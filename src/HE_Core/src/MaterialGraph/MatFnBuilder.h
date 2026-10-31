// The thin builder the engine's material functions are written with: a MaterialGraph with
// column bookkeeping so the function opens tidy in the editor. Private to HE_Core's
// MaterialGraph sources (the weather and the procedural surface functions); matfn_gen has
// its own copy for the UI effects.
#pragma once

#include <MaterialGraph/MaterialGraph.h>

#include <algorithm>
#include <initializer_list>
#include <vector>

namespace HE::MatFnBuild
{
using T = MatNodeType;
using P = MatPinType;

inline constexpr float kColStep = 230.0f;
inline constexpr float kRowStep = 110.0f;

struct Fn
{
    MaterialGraph g;
    int inRow = 0;

    // Inputs first and in display order: a call node's pins are its FnInput nodes by id.
    int in(const char* name, P type, float def)
    {
        const int id = g.addNode(T::FnInput, 0.0f, static_cast<float>(inRow++) * kRowStep);
        MatGraphNode& n = *g.findNode(id);
        n.s    = name;
        n.p[0] = static_cast<float>(type);
        n.p[1] = def;
        n.p[2] = 1.0f;   // the default is authored (see the FnInput registry entry)
        return id;
    }
    int node(T type, int col, float row = 0.0f)
    {
        return g.addNode(type, static_cast<float>(col) * kColStep, row * kRowStep);
    }
    int k(float v, int col, float row = 0.0f)
    {
        const int id = node(T::ConstFloat, col, row);
        g.findNode(id)->p[0] = v;
        return id;
    }
    int out(const char* name, P type, int col, float row = 0.0f)
    {
        const int id = node(T::FnOutput, col, row);
        MatGraphNode& n = *g.findNode(id);
        n.s    = name;
        n.p[0] = static_cast<float>(type);
        return id;
    }
    void link(int src, int srcPin, int dst, int dstPin) { g.connect(src, srcPin, dst, dstPin); }

    // ── Expression style, for the bigger functions ───────────────────────────────
    // R is one output pin of a node. e() places a node one column right of its deepest
    // input and wires the inputs in pin order; a constant sits in column 1. The graph is
    // exactly the one the explicit calls above would build, only the bookkeeping is done.
    struct R { int node = -1; int pin = 0; };

    R at(R r, int pin) const { return { r.node, pin }; }
    R e(T type, std::initializer_list<R> inputs)
    {
        int col = 1;
        for (const R& r : inputs) col = std::max(col, colOf(r.node) + 1);
        const int id = node(type, col, static_cast<float>(nextRow(col)));
        setCol(id, col);
        int pin = 0;
        for (const R& r : inputs) g.connect(r.node, r.pin, id, pin++);
        return { id, 0 };
    }
    R c(float v)
    {
        const int id = node(T::ConstFloat, 1, static_cast<float>(nextRow(1)));
        g.findNode(id)->p[0] = v;
        setCol(id, 1);
        return { id, 0 };
    }
    // A function-interface node (FnInput) as an operand; its column is 0.
    R input(int id) { setCol(id, 0); return { id, 0 }; }
    // An FnOutput one column right of its source.
    int result(const char* name, P type, R src)
    {
        const int col = colOf(src.node) + 1;
        const int id = out(name, type, col, static_cast<float>(nextRow(col)));
        setCol(id, col);
        g.connect(src.node, src.pin, id, 0);
        return id;
    }

private:
    std::vector<int> cols_, rows_;   // by node id / by column
    int colOf(int id) const { return id >= 0 && id < static_cast<int>(cols_.size()) ? cols_[id] : 0; }
    void setCol(int id, int col)
    {
        if (id >= static_cast<int>(cols_.size())) cols_.resize(id + 1, 0);
        cols_[id] = col;
    }
    int nextRow(int col)
    {
        if (col >= static_cast<int>(rows_.size())) rows_.resize(col + 1, 0);
        return rows_[col]++;
    }
};
} // namespace HE::MatFnBuild
