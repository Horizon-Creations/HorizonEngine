#pragma once

#include "DocsLibrary.h"

#include <string>
#include <vector>

// ── The guides, in the editor ────────────────────────────────────────────────
// The third generated part of the manual, next to the node reference
// (HcNodeReference) and the control reference (EditorReference). Neither of
// those answers the question people actually open the manual with. The node
// reference says what a node's pins are; the control reference says what a
// field does. Nobody arrives asking either. They arrive asking "how do I make
// an enemy chase the player", and the answer to that is a RECIPE: what you have
// at the end, which steps in which order, and where it silently does nothing.
//
// So this is not a chapter about how navigation works. It is a page per thing
// somebody wants to have built, written against the editor as it is today.
//
// Three rules the pages here live by, because breaking any of them makes a
// guide worse than no guide at all:
//
//   1. EVERY name is a name the reader can see. A button, a field, a component,
//      a node — spelled the way the editor spells it, checked against the code
//      that draws it, not against memory. A step that says "press Rebuild" when
//      the button says "Bake" costs more time than it saves.
//   2. EVERY guide names the silent failures. This engine has plenty of places
//      that do not crash, do not warn, and do not work; those are what the
//      reader is actually stuck on, and they belong in Warning callouts next to
//      the step that triggers them.
//   3. WHAT IS MISSING IS SAID OUT LOUD, in one sentence, with the detour —
//      never left for the reader to discover by failing.
//
// The pages ship WITH the editor: unlike the manual proper, they do not come
// from the website bundle, so they cannot be older than the build reading them
// and they are there before anything has been downloaded.

namespace HE::Ed::Guides
{

// The sidebar heading these pages sit under. It is drawn FIRST, ahead of the
// bundle's own groups — the library has no way to insert a group (appendPage
// can only extend the last one), so DocsPanel's sidebar assembles the order and
// this is the name it looks for.
inline constexpr const char* kGroupTitle = "Guides";

// ── The four content tracks ──────────────────────────────────────────────────
// One file each under Guides/, so four people can write four sets of guides
// without ever touching the same translation unit. Each returns its finished
// pages; install() puts them in the library in the order below, which is also
// the order they appear in the sidebar.
//
// A track that has nothing yet returns an empty vector — that is the shipped
// state of a new file, and it compiles and runs.
//
// Page ids must start with "guides-" so they cannot collide with a page from
// the website bundle, and must stay stable once written: an id is half of a
// topic reference, and a renamed page breaks every link into it.
std::vector<Docs::Page> firstStepsPages();   // Guides/GuidesFirstSteps.cpp
std::vector<Docs::Page> characterPages();    // Guides/GuidesCharacter.cpp
std::vector<Docs::Page> worldPages();        // Guides/GuidesWorld.cpp
std::vector<Docs::Page> shippingPages();     // Guides/GuidesShipping.cpp

// Build every guide page and put it in the library, replacing earlier copies.
// Idempotent, like the node and control references — a second reader, or a
// test, must not end up with two of everything.
void install(Docs::Library& lib);

// The page ids install() last registered, in sidebar order. This is what lets
// the panel draw a "Guides" heading over exactly these pages and leave them out
// of the group appendPage dropped them into. Empty until install() has run,
// which is why the sidebar has to tolerate an empty list.
const std::vector<std::string>& pageIds();

// ── Writing a guide ──────────────────────────────────────────────────────────
// Content is DATA, and building it by filling Docs::Block fields by hand is
// both noisy and easy to get subtly wrong. These are the whole surface a track
// needs.
//
// The one that matters structurally is section(): it fills Section::text, the
// flat string the search index is built from, by walking every block —
// paragraphs, list items, table cells, code, the label AND sub of every Flow
// step, and recursively into a callout's body. A section built any other way is
// in the manual but not in the search, and nothing anywhere reports it. So
// never assign Section::text by hand; always come through section().

Docs::Run run(std::string text, Docs::Style style = Docs::Style::Body,
              std::string href = {});

// A link to another topic in the manual — "guides-npc-chase#navmesh", or a page
// id on its own. Resolved when the reader clicks, so it does not matter whether
// the target has been installed yet.
Docs::Run link(std::string text, std::string topic);

Docs::Block para(std::string text);
// A paragraph with mixed styling — code spans, bold terms, links.
Docs::Block rich(std::vector<Docs::Run> runs);
// The section's opening sentence, drawn larger. One per section, at the top.
Docs::Block lead(std::string text);
Docs::Block heading(std::string text);
Docs::Block bullets(std::vector<std::string> items);
Docs::Block numbers(std::vector<std::string> items);

// The ordered steps of a recipe, drawn as a chain of boxes with arrows between
// them. `label` is the action ("Press Bake"), `sub` the detail. This is the
// block a guide is mostly made of.
Docs::Block steps(std::vector<Docs::Block::Step> s);

// `title` may be empty, in which case the code block is drawn without a caption.
Docs::Block code(std::string title, std::string body);
// `head` may be empty for a table without a header row. Every row should have
// as many cells as the header does.
Docs::Block table(std::vector<std::string> head,
                  std::vector<std::vector<std::string>> rows);

// A boxed aside. The title is drawn as a bold first line inside the box — the
// renderer does not use Block::title for callouts, so it has to be a block.
Docs::Block callout(Docs::Tone tone, std::string title,
                    std::vector<Docs::Block> body);
// warn() is for the silent failures: it does not crash, it does not warn, it
// just does not work. Use it generously — it is the reason these pages exist.
Docs::Block warn(std::string title, std::vector<Docs::Block> body);
Docs::Block tip(std::string title, std::vector<Docs::Block> body);
Docs::Block note(std::string title, std::vector<Docs::Block> body);

// `id` is the anchor F1 and every cross-link addresses; unique within the page
// and stable forever. `eyebrow` is the small label above the heading — in these
// pages it is normally the step number ("Step 3") or the kind of section
// ("Guide", "Troubleshooting").
Docs::Section section(std::string id, std::string eyebrow, std::string title,
                      std::vector<Docs::Block> blocks);

// The summary is what the reader sees before opening the page, so it should say
// what they will HAVE at the end, not what the page is about.
Docs::Page page(std::string id, std::string title, std::string summary,
                std::vector<Docs::Section> sections);

} // namespace HE::Ed::Guides
