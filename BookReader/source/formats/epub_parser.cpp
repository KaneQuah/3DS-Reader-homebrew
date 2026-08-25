#include "formats/epub_parser.h"
#include "trace.h"
#include <pugixml.hpp>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <cctype>

const std::string EpubDocument::kEmpty;

//=============================================================================
// XHTML text extraction: strip tags, decode entities, extract visible text
//=============================================================================
namespace {

// Block tags that should be followed by a paragraph break
const char* const BLOCK_TAGS[] = {
    "p","h1","h2","h3","h4","h5","h6","div","blockquote","li","pre",nullptr
};
// Elements whose text is metadata, not prose, and must never reach the page.
// <desc> is the one that matters in practice: image-only cover pages wrap a
// JPEG in an <svg> and put a bare URL in <desc>, which would otherwise be
// rendered as the entire visible content of the book's first page.
const char* const SKIP_TAGS[] = {
    "script","style","svg","desc","title","head","meta","link",nullptr
};

/// ASCII case-insensitive compare against a lowercase literal. Replaces
/// `std::string tag = child.name(); std::transform(::tolower)`, which
/// allocated and lower-cased a std::string for every element in the document
/// — twice per element once both tag lists were consulted.
inline bool tagEquals(const char* tag, const char* lower) {
    for (;; tag++, lower++) {
        char c = *tag;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != *lower) return false;
        if (c == 0) return true;
    }
}

inline bool inTagList(const char* const* list, const char* tag) {
    for (int i = 0; list[i]; i++)
        if (tagEquals(tag, list[i])) return true;
    return false;
}

/// Depth-first text extraction. A plain recursive function rather than the
/// std::function-typed lambda this used to be: std::function goes through an
/// indirect call and a heap-allocated closure on every one of the thousands of
/// nodes in a chapter, and cannot be inlined.
void walkForText(pugi::xml_node node, std::string& out) {
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        pugi::xml_node_type t = child.type();
        if (t == pugi::node_pcdata || t == pugi::node_cdata) {
            out += child.value();
        } else if (t == pugi::node_element) {
            const char* tag = child.name();
            if (tagEquals(tag, "br"))            { out += "\n"; }
            else if (inTagList(SKIP_TAGS, tag))  { /* metadata — skip */ }
            else {
                walkForText(child, out);
                if (inTagList(BLOCK_TAGS, tag)) out += "\n\n";
            }
        }
    }
}

/// First <body> element anywhere in the document, found by walking rather than
/// by XPath.
///
/// `doc.select_node("//body")` compiles an XPath expression and then evaluates
/// it over the entire node set on every chapter load. It is also the one
/// pugixml API here that allocates through its own exception-free error path,
/// which this build (-fno-exceptions, PUGIXML_NO_EXCEPTIONS) cannot report on.
/// In practice <body> is a child of the root <html>, so the common case is two
/// pointer hops; the recursive fallback covers documents with a wrapper
/// element or an XML declaration in the way.
pugi::xml_node findBody(pugi::xml_node node) {
    for (pugi::xml_node child = node.first_child(); child; child = child.next_sibling()) {
        if (child.type() != pugi::node_element) continue;
        if (tagEquals(child.name(), "body")) return child;
        pugi::xml_node deeper = findBody(child);
        if (deeper) return deeper;
    }
    return pugi::xml_node();
}

} // namespace

std::string epubExtractTextFromXhtml(const std::string& xhtml) {
    pugi::xml_document doc;
    // EPUB XHTML may have <!DOCTYPE> and namespace attributes
    pugi::xml_parse_result res = doc.load_string(xhtml.c_str(),
        pugi::parse_default | pugi::parse_doctype);

    // Malformed XHTML: return empty rather than walking a half-built tree.
    // Many real-world EPUBs contain sloppy markup, so the caller treats an
    // empty chapter as "skip" rather than a fatal error.
    if (!res) return std::string();

    std::string out;
    // The extracted text is always shorter than the markup it came from, and
    // usually by a lot — but growing a string from empty to ~100 KB means a
    // dozen reallocate-and-copy rounds. Half the markup size is a safe
    // over-estimate that costs one allocation.
    out.reserve(xhtml.size() / 2 + 64);

    // Start at <body>, not the document root: walking from the root pulls in
    // <head><title>, so every chapter's title text was being prepended to its
    // own prose. Fall back to the root for fragments that have no <body>.
    pugi::xml_node body = findBody(doc);
    walkForText(body ? body : doc.root(), out);
    return out;
}

//-----------------------------------------------------------------------------
// Count non-whitespace characters — used to decide whether a chapter carries
// enough prose to be worth showing.
//-----------------------------------------------------------------------------
size_t epubCountVisibleChars(const std::string& s, size_t stopAt) {
    size_t n = 0;
    for (unsigned char c : s) {
        if (!isspace(c) && ++n >= stopAt) break;
    }
    return n;
}

//=============================================================================
// EpubDocument — lazy chapter access
//=============================================================================
const std::string& EpubDocument::chapterText(int index) {
    if (index < 0 || index >= (int)chapters.size()) return kEmpty;

    for (auto& slot : cache_)
        if (slot.index == index) return slot.text;

    TRACE("chapterText(%d) href=%s", index, chapters[(size_t)index].href.c_str());

    std::string xhtml;
    if (!zip_.read(chapters[(size_t)index].href, xhtml)) {
        TRACE("  zip read FAILED");
        return kEmpty;
    }
    TRACE("  inflated %u bytes", (unsigned)xhtml.size());

    // Round-robin eviction. With three slots and sequential reading, the
    // chapter being replaced is always the one furthest from the cursor.
    CacheSlot& slot = cache_[cacheNext_];
    cacheNext_      = (cacheNext_ + 1) % CACHE_SIZE;
    slot.index      = index;
    slot.text       = epubExtractTextFromXhtml(xhtml);
    TRACE("  extracted %u chars of text", (unsigned)slot.text.size());
    return slot.text;
}

void EpubDocument::dropCache() {
    for (auto& slot : cache_) {
        slot.index = -1;
        slot.text.clear();
        slot.text.shrink_to_fit();
    }
    cacheNext_ = 0;
}

void EpubDocument::close() {
    dropCache();
    chapters.clear();
    meta = {};
    zip_.close();
}

//=============================================================================
// EpubParser
//=============================================================================

//-----------------------------------------------------------------------------
// OPF path from META-INF/container.xml
//-----------------------------------------------------------------------------
std::string EpubParser::parseOPFPath(const std::string& xml) {
    pugi::xml_document doc;
    if (doc.load_string(xml.c_str()).status != pugi::status_ok) return "";
    return doc.select_node("//rootfile").node().attribute("full-path").value();
}

//-----------------------------------------------------------------------------
// OPF parsing: metadata, manifest, spine
//-----------------------------------------------------------------------------
bool EpubParser::parseOPF(const std::string& xml, const std::string& opfDir,
                           std::vector<EpubItem>& manifest,
                           std::vector<std::string>& spine,
                           EpubMetadata& meta, std::string& tocId) {
    pugi::xml_document doc;
    if (doc.load_string(xml.c_str()).status != pugi::status_ok) return false;

    auto pkg = doc.child("package");

    // Metadata
    auto md = pkg.child("metadata");
    meta.title    = md.child("dc:title").text().get();
    meta.author   = md.child("dc:creator").text().get();
    meta.language = md.child("dc:language").text().get();

    // Manifest
    for (auto item : pkg.child("manifest").children("item")) {
        EpubItem ei;
        ei.id        = item.attribute("id").value();
        ei.href      = opfDir.empty() ? item.attribute("href").value()
                                      : opfDir + "/" + item.attribute("href").value();
        ei.mediaType = item.attribute("media-type").value();
        // EPUB 3 nav document takes priority over an EPUB 2 NCX
        if (std::string(item.attribute("properties").value()) == "nav")
            tocId = ei.id;
        if (ei.mediaType == "application/x-dtbncx+xml" && tocId.empty())
            tocId = ei.id;
        manifest.push_back(ei);
    }

    // Spine: ordered list of idrefs into the manifest
    auto spineNode = pkg.child("spine");
    for (auto itemref : spineNode.children("itemref"))
        spine.push_back(itemref.attribute("idref").value());

    // Fall back to the spine's toc attribute if the manifest named neither a
    // nav document nor an NCX.
    if (tocId.empty())
        tocId = spineNode.attribute("toc").value();

    return !spine.empty();
}

//-----------------------------------------------------------------------------
// NCX TOC parser (EPUB 2)
//-----------------------------------------------------------------------------
bool EpubParser::parseNCX(const std::string& xml,
                           std::vector<std::string>& ids,
                           std::vector<std::string>& titles) {
    pugi::xml_document doc;
    if (doc.load_string(xml.c_str()).status != pugi::status_ok) return false;
    for (auto np : doc.select_nodes("//navPoint")) {
        auto n = np.node();
        std::string src = n.child("content").attribute("src").value();
        auto hash = src.find('#');
        if (hash != std::string::npos) src = src.substr(0, hash);
        ids.push_back(src);
        titles.push_back(n.child("navLabel").child("text").text().get());
    }
    return true;
}

//-----------------------------------------------------------------------------
// Navigation document parser (EPUB 3)
//-----------------------------------------------------------------------------
bool EpubParser::parseNavXhtml(const std::string& xml,
                                std::vector<std::string>& ids,
                                std::vector<std::string>& titles) {
    pugi::xml_document doc;
    if (doc.load_string(xml.c_str()).status != pugi::status_ok) return false;
    for (auto a : doc.select_nodes("//nav//a")) {
        auto n = a.node();
        std::string href = n.attribute("href").value();
        auto hash = href.find('#');
        if (hash != std::string::npos) href = href.substr(0, hash);
        ids.push_back(href);
        titles.push_back(n.text().get());
    }
    return true;
}

//-----------------------------------------------------------------------------
// open — reads structure only. Chapter bodies stay in the ZIP until asked for.
//-----------------------------------------------------------------------------
EpubParser::Result EpubParser::open(const std::string& epubPath, EpubDocument& out) {
    out.close();
    TRACE("EpubParser::open %s", epubPath.c_str());

    // Step 1: index the archive
    if (!out.zip_.open(epubPath)) { TRACE("  zip open FAILED"); return Result::BadZip; }
    TRACE("  zip indexed");

    // Step 2: confirm mimetype
    std::string mimetype;
    if (!out.zip_.read("mimetype", mimetype)) return Result::BadZip;
    while (!mimetype.empty() &&
           (mimetype.back() == '\n' || mimetype.back() == '\r' || mimetype.back() == ' '))
        mimetype.pop_back();
    if (mimetype != "application/epub+zip") return Result::NotEpub;

    // Step 3: container.xml → OPF path
    std::string containerXml;
    if (!out.zip_.read("META-INF/container.xml", containerXml)) return Result::BadZip;
    std::string opfPath = parseOPFPath(containerXml);
    if (opfPath.empty()) return Result::BadOPF;

    std::string opfDir;
    auto slash = opfPath.rfind('/');
    if (slash != std::string::npos) opfDir = opfPath.substr(0, slash);

    // Step 4: OPF → metadata, manifest, spine
    std::string opfXml;
    if (!out.zip_.read(opfPath, opfXml)) return Result::BadOPF;

    TRACE("  OPF %s (%u bytes)", opfPath.c_str(), (unsigned)opfXml.size());

    std::vector<EpubItem>    manifest;
    std::vector<std::string> spineIds;
    std::string              tocId;
    if (!parseOPF(opfXml, opfDir, manifest, spineIds, out.meta, tocId))
        return Result::BadOPF;
    TRACE("  OPF parsed: %u manifest, %u spine, tocId=%s",
          (unsigned)manifest.size(), (unsigned)spineIds.size(), tocId.c_str());

    std::unordered_map<std::string, const EpubItem*> idMap;
    idMap.reserve(manifest.size());
    for (const auto& item : manifest) idMap[item.id] = &item;

    // Step 5: TOC titles (optional — a book without one is still readable)
    std::vector<std::string> tocSrcs, tocTitles;
    if (!tocId.empty()) {
        auto it = idMap.find(tocId);
        if (it != idMap.end()) {
            std::string tocXml;
            if (out.zip_.read(it->second->href, tocXml)) {
                if (it->second->mediaType == "application/x-dtbncx+xml")
                    parseNCX(tocXml, tocSrcs, tocTitles);
                else
                    parseNavXhtml(tocXml, tocSrcs, tocTitles);
            }
        }
    }

    // Step 6: build the chapter list — hrefs and titles only, no bodies
    out.chapters.clear();
    out.chapters.reserve(spineIds.size());

    for (const auto& idref : spineIds) {
        auto it = idMap.find(idref);
        if (it == idMap.end()) continue;
        const EpubItem& item = *it->second;

        EpubChapter chap;
        chap.id   = item.id;
        chap.href = item.href;
        for (size_t i = 0; i < tocSrcs.size(); i++) {
            if (!tocSrcs[i].empty() &&
                item.href.find(tocSrcs[i]) != std::string::npos) {
                chap.title = tocTitles[i];
                break;
            }
        }
        out.chapters.push_back(std::move(chap));
    }

    TRACE("  built %u chapters", (unsigned)out.chapters.size());
    return out.chapters.empty() ? Result::BadContent : Result::OK;
}
