#include "AssimpMeshImport.h"
#include <algorithm>
#include <cctype>
#include <cstring>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace Importer
{

namespace
{
std::string lowerExtension(const std::filesystem::path& p)
{
	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
	               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext;
}

// aiMatrix4x4 is ROW-major (a1 a2 a3 a4 is the first row); glm::mat4 is
// column-major. make_mat4 over the raw floats therefore yields the transpose.
glm::mat4 toGlm(const aiMatrix4x4& m)
{
	return glm::transpose(glm::make_mat4(&m.a1));
}

// Appends one mesh instance (its vertices transformed by `world`, positions
// scaled by `scale`), rebasing the indices onto the vertices just appended.
// Only triangles are taken — after aiProcess_Triangulate + SortByPType a mesh
// holds one primitive type, and points/lines were removed, so a non-triangle
// mesh here is one to skip whole rather than one to pick faces out of.
void appendMesh(const aiMesh& mesh, const glm::mat4& world, float scale,
                AssimpBakedGeometry& out)
{
	if (mesh.mNumVertices == 0 || !(mesh.mPrimitiveTypes & aiPrimitiveType_TRIANGLE))
		return;

	const auto      baseVertex = static_cast<uint32_t>(out.positions.size() / 3);
	const auto      indexStart = static_cast<uint32_t>(out.indices.size());
	const glm::mat3 normalMat  = glm::transpose(glm::inverse(glm::mat3(world)));
	const bool      hasNormals = mesh.HasNormals();
	const bool      hasUvs     = mesh.HasTextureCoords(0);

	for (unsigned v = 0; v < mesh.mNumVertices; ++v)
	{
		const aiVector3D& p  = mesh.mVertices[v];
		const glm::vec3   wp = glm::vec3(world * glm::vec4(p.x, p.y, p.z, 1.0f)) * scale;
		out.positions.insert(out.positions.end(), { wp.x, wp.y, wp.z });

		if (hasNormals)
		{
			const aiVector3D& n  = mesh.mNormals[v];
			glm::vec3         wn = normalMat * glm::vec3(n.x, n.y, n.z);
			// A degenerate source normal (FBX exporters write them for unused
			// vertices) must not become NaN in the asset; leave it for generation.
			if (glm::dot(wn, wn) > 1e-12f) wn = glm::normalize(wn);
			else { wn = glm::vec3(0.0f); out.missingNormals = true; }
			out.normals.insert(out.normals.end(), { wn.x, wn.y, wn.z });
		}
		else
		{
			out.normals.insert(out.normals.end(), { 0.0f, 0.0f, 0.0f });
			out.missingNormals = true;
		}

		if (hasUvs)
		{
			// Assimp's UV origin is BOTTOM-left, the engine's convention (the
			// glTF path flips V because glTF's origin is top-left; here nothing
			// is flipped — see the OBJ fixture test, which pins this).
			const aiVector3D& uv = mesh.mTextureCoords[0][v];
			out.uvs.insert(out.uvs.end(), { uv.x, uv.y });
		}
		else
			out.uvs.insert(out.uvs.end(), { 0.0f, 0.0f });
	}

	for (unsigned f = 0; f < mesh.mNumFaces; ++f)
	{
		const aiFace& face = mesh.mFaces[f];
		if (face.mNumIndices != 3)
			continue;
		for (unsigned k = 0; k < 3; ++k)
			out.indices.push_back(baseVertex + face.mIndices[k]);
	}

	const auto indexCount = static_cast<uint32_t>(out.indices.size()) - indexStart;
	if (indexCount == 0)
	{
		// Nothing triangular after all: drop the vertices again so the streams
		// hold no orphans and the range table describes the buffer exactly.
		out.positions.resize(static_cast<size_t>(baseVertex) * 3);
		out.normals.resize(static_cast<size_t>(baseVertex) * 3);
		out.uvs.resize(static_cast<size_t>(baseVertex) * 2);
		return;
	}
	out.ranges.push_back({ indexStart, indexCount, static_cast<int>(mesh.mMaterialIndex) });
	if (mesh.HasBones())
		out.skinned = true;
}

// FBX's UnitScaleFactor: how many centimetres one file unit is (1 = cm, the
// SDK default; 100 = m; 2.54 = inches). Assimp writes it into the scene
// metadata as a FLOAT (FBXDocument.h: fbx_simple_property(UnitScaleFactor,
// float, 1)); the double is tried as well in case a later Assimp changes the
// type — see fbxUnitScale's caller for why that would have to be re-checked.
float fbxUnitScaleFactor(const aiScene& scene)
{
	float  usf  = 1.0f;
	double usfD = 1.0;
	if (scene.mMetaData && !scene.mMetaData->Get("UnitScaleFactor", usf)
	    && scene.mMetaData->Get("UnitScaleFactor", usfD))
		usf = static_cast<float>(usfD);
	return usf > 0.0f ? usf : 1.0f;
}

void walkNodes(const aiScene& scene, const aiNode& node, const glm::mat4& parent,
               float scale, AssimpBakedGeometry& out)
{
	// The root's own transformation is part of the product: that is where
	// Assimp's FBX and COLLADA loaders put the axis and unit corrections.
	const glm::mat4 world = parent * toGlm(node.mTransformation);
	for (unsigned i = 0; i < node.mNumMeshes; ++i)
		appendMesh(*scene.mMeshes[node.mMeshes[i]], world, scale, out);
	for (unsigned c = 0; c < node.mNumChildren; ++c)
		walkNodes(scene, *node.mChildren[c], world, scale, out);
}
} // namespace

bool isAssimpSource(const std::filesystem::path& sourcePath)
{
	const std::string ext = lowerExtension(sourcePath);
	return ext == ".fbx" || ext == ".obj" || ext == ".dae";
}

struct AssimpScene::Impl
{
	Assimp::Importer importer;   // owns the aiScene for as long as this lives
	const aiScene*   scene = nullptr;
};

AssimpScene::AssimpScene() : impl_(std::make_unique<Impl>()) {}
AssimpScene::~AssimpScene() = default;

const aiScene* AssimpScene::scene() const { return impl_->scene; }

bool AssimpScene::load(const std::filesystem::path& sourcePath, std::string& error)
{
	fbx_ = lowerExtension(sourcePath) == ".fbx";

	// SortByPType splits mixed meshes by primitive type; with points and lines
	// on the removal list only triangle meshes survive, which is all a static
	// mesh can hold. NOT requested, deliberately:
	//   GlobalScale        — would apply FBX's UnitScaleFactor a second time on
	//                        top of the root transform (see bake()).
	//   FlipUVs            — Assimp's origin already matches the engine's.
	//   PreTransformVertices — collapses the node tree AND drops the per-mesh
	//                        material split we build sections from.
	//   GenNormals         — MeshImporter generates missing normals itself, the
	//                        same way for every format.
	impl_->importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
	                                   aiPrimitiveType_POINT | aiPrimitiveType_LINE);
	const unsigned flags = aiProcess_Triangulate
	                     | aiProcess_JoinIdenticalVertices
	                     | aiProcess_SortByPType;
	impl_->scene = impl_->importer.ReadFile(sourcePath.string(), flags);
	if (!impl_->scene)
	{
		error = impl_->importer.GetErrorString();
		if (error.empty()) error = "Assimp returned no scene";
		return false;
	}
	return true;
}

bool AssimpScene::bake(float uniformScale, AssimpBakedGeometry& out) const
{
	const aiScene* scene = impl_->scene;
	if (!scene)
		return false;

	// FBX arrives in the FILE's units, and the file says what they are:
	// UnitScaleFactor centimetres per unit. So metres = units × USF × 0.01.
	//
	// Assimp (v6.0.5) MEANS to do this itself — FBXConverter::correctRootTransform
	// multiplies axes × UnitScaleFactor into the root node — but reads the
	// factor back out of the metadata as a double while it was stored as a
	// float, so the read fails, the factor stays 1 and only the axes land in
	// the root. Were that ever fixed upstream, this line would scale twice; the
	// metre-FBX fixture in test_assimpimport pins it, so an Assimp upgrade that
	// changes it fails loudly there rather than shipping 100× meshes.
	// The factor multiplies the caller's scale rather than replacing it.
	const float scale = uniformScale * (fbx_ ? fbxUnitScaleFactor(*scene) * 0.01f : 1.0f);

	if (scene->mRootNode)
		walkNodes(*scene, *scene->mRootNode, glm::mat4(1.0f), scale, out);
	// No node references a mesh: take the meshes directly, untransformed.
	if (out.positions.empty())
		for (unsigned m = 0; m < scene->mNumMeshes; ++m)
			appendMesh(*scene->mMeshes[m], glm::mat4(1.0f), scale, out);

	return !out.positions.empty();
}

int AssimpScene::primaryMaterialIndex(const AssimpBakedGeometry& baked) const
{
	if (!baked.ranges.empty())
		return baked.ranges.front().materialIndex;
	const aiScene* scene = impl_->scene;
	return (scene && scene->mNumMaterials > 0) ? 0 : BakedRange::kNoMaterial;
}

} // namespace Importer
