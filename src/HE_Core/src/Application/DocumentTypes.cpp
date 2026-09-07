#include <Application/DocumentTypes.h>
#include <algorithm>

namespace HE {

namespace
{
    std::string xmlEscape(const std::string& s)
    {
        std::string out;
        for (char c : s)
            switch (c)
            {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:   out += c;
            }
        return out;
    }

    // A .reg file is UTF-16-ish text with C-style escapes in its values; the only
    // two that matter for a path are the backslash and the quote.
    std::string regEscape(const std::string& s)
    {
        std::string out;
        for (char c : s)
        {
            if (c == '\\' || c == '"') out += '\\';
            out += c;
        }
        return out;
    }

    // The system's own name for the type. Derived, never asked for: a UTI and a
    // MIME type that disagree with the bundle identifier are how a file ends up
    // opening in the wrong application.
    std::string utiFor(const std::string& bundleId, const std::string& ext)
    {
        return bundleId + "." + ext;
    }
    std::string mimeFor(const std::string& bundleId, const std::string& ext)
    {
        // "application/x-<bundle-with-dashes>-<ext>" keeps it inside the x- space,
        // where a vendor may invent names without registering them.
        std::string id = bundleId;
        std::replace(id.begin(), id.end(), '.', '-');
        return "application/x-" + id + "-" + ext;
    }
}

bool heValidDocumentExtension(const std::string& ext)
{
    if (ext.empty() || ext.size() > 12) return false;
    for (char c : ext)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    // A type whose name starts with a digit is a UTI nobody can write down.
    return !(ext[0] >= '0' && ext[0] <= '9');
}

std::string heInfoPlistDocumentTypes(const std::vector<AppDocumentType>& types,
                                     const std::string& bundleId)
{
    std::vector<AppDocumentType> valid;
    for (const AppDocumentType& t : types)
        if (heValidDocumentExtension(t.extension)) valid.push_back(t);
    if (valid.empty()) return {};

    std::string out;
    // What the application OPENS…
    out += "  <key>CFBundleDocumentTypes</key>\n  <array>\n";
    for (const AppDocumentType& t : valid)
    {
        out += "    <dict>\n";
        out += "      <key>CFBundleTypeName</key><string>" + xmlEscape(t.displayName) + "</string>\n";
        out += "      <key>CFBundleTypeRole</key><string>Editor</string>\n";
        out += "      <key>LSHandlerRank</key><string>Owner</string>\n";
        // The icon is named without its extension, the way CFBundleIconFile is.
        out += "      <key>CFBundleTypeIconFile</key><string>Doc-" + t.extension + "</string>\n";
        out += "      <key>LSItemContentTypes</key>\n      <array><string>"
             + xmlEscape(utiFor(bundleId, t.extension)) + "</string></array>\n";
        out += "    </dict>\n";
    }
    out += "  </array>\n";

    // …and what it INVENTED. Without this the UTI above is a name the system has
    // never heard of, and it will not associate anything with it.
    out += "  <key>UTExportedTypeDeclarations</key>\n  <array>\n";
    for (const AppDocumentType& t : valid)
    {
        out += "    <dict>\n";
        out += "      <key>UTTypeIdentifier</key><string>"
             + xmlEscape(utiFor(bundleId, t.extension)) + "</string>\n";
        out += "      <key>UTTypeDescription</key><string>" + xmlEscape(t.displayName) + "</string>\n";
        out += "      <key>UTTypeConformsTo</key>\n      <array><string>public.data</string></array>\n";
        out += "      <key>UTTypeIconFile</key><string>Doc-" + t.extension + "</string>\n";
        out += "      <key>UTTypeTagSpecification</key>\n      <dict>\n";
        out += "        <key>public.filename-extension</key>\n        <array><string>"
             + t.extension + "</string></array>\n";
        out += "        <key>public.mime-type</key>\n        <array><string>"
             + mimeFor(bundleId, t.extension) + "</string></array>\n";
        out += "      </dict>\n";
        out += "    </dict>\n";
    }
    out += "  </array>\n";
    return out;
}

std::string heDesktopEntry(const std::string& appName, const std::string& exeName,
                           const std::string& bundleId,
                           const std::vector<AppDocumentType>& types)
{
    std::string mimes;
    for (const AppDocumentType& t : types)
        if (heValidDocumentExtension(t.extension))
            mimes += mimeFor(bundleId, t.extension) + ";";

    std::string out;
    out += "[Desktop Entry]\n";
    out += "Type=Application\n";
    out += "Name=" + appName + "\n";
    // %F, not %f: the application is handed every file at once, which is what
    // opening a selection means, and it is what the argv path below reads.
    out += "Exec=" + exeName + " %F\n";
    out += "Icon=" + bundleId + "\n";
    out += "Terminal=false\n";
    if (!mimes.empty()) out += "MimeType=" + mimes + "\n";
    return out;
}

std::string heSharedMimeInfo(const std::string& bundleId,
                             const std::vector<AppDocumentType>& types)
{
    std::string out;
    out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    out += "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";
    for (const AppDocumentType& t : types)
    {
        if (!heValidDocumentExtension(t.extension)) continue;
        out += "  <mime-type type=\"" + mimeFor(bundleId, t.extension) + "\">\n";
        out += "    <comment>" + xmlEscape(t.displayName) + "</comment>\n";
        out += "    <glob pattern=\"*." + t.extension + "\"/>\n";
        out += "  </mime-type>\n";
    }
    out += "</mime-info>\n";
    return out;
}

std::string heWindowsRegistration(const std::string& appName, const std::string& exeName,
                                  const std::string& bundleId,
                                  const std::vector<AppDocumentType>& types)
{
    std::string out;
    out += "Windows Registry Editor Version 5.00\n";
    out += "\n";
    out += "; Generated by Horizon Engine. Applies to the CURRENT USER only, so it\n";
    out += "; needs no administrator; run it after copying the application to where\n";
    out += "; it will live, because the paths below are the ones it has now.\n";
    for (const AppDocumentType& t : types)
    {
        if (!heValidDocumentExtension(t.extension)) continue;
        // The ProgId is the application's own name for the type, the same shape
        // the UTI has on macOS, so the three declarations stay recognisably one.
        const std::string progId = bundleId + "." + t.extension;
        out += "\n[HKEY_CURRENT_USER\\Software\\Classes\\." + t.extension + "]\n";
        out += "@=\"" + progId + "\"\n";
        out += "\n[HKEY_CURRENT_USER\\Software\\Classes\\" + progId + "]\n";
        out += "@=\"" + regEscape(t.displayName) + "\"\n";
        out += "\n[HKEY_CURRENT_USER\\Software\\Classes\\" + progId + "\\DefaultIcon]\n";
        out += "@=\"%~dp0" + regEscape(exeName) + ",0\"\n";
        out += "\n[HKEY_CURRENT_USER\\Software\\Classes\\" + progId + "\\shell\\open\\command]\n";
        out += "@=\"\\\"%~dp0" + regEscape(exeName) + "\\\" \\\"%1\\\"\"\n";
    }
    out += "\n; " + appName + "\n";
    return out;
}

} // namespace HE
