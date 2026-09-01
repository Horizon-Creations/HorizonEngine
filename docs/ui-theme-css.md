# Themes written as CSS — what would fit, what would not

Assessment, 01.09.2026. Nothing here is built. The question was whether a theme
could later be authored as CSS instead of in the Theme editor, and how far that
goes; this is the answer, written down before anybody starts.

The short version: **most of it is a parser, not an engine change.** The theme
model that exists today is already a strict subset of CSS's model — selectors
that layer by specificity, declarations that are property/value pairs. Three
things genuinely do not map, and each has one honest answer.

## What maps one to one

| CSS | Theme today |
|---|---|
| `Button { … }` | a style keyed `"Button"` |
| `.card { … }` | a style keyed `"Card"` |
| `Button.success { … }` | a style keyed `"Button.success"` |
| specificity 0,0,1 < 0,1,0 < 0,1,1 | `uiThemeStylesFor`'s order, deliberately taken from CSS |
| later declaration wins per property | `uiThemeValueFor` keeps the last hit |
| `class="card"` on an element | `UIElement::themeStyle` |

The cascade is the part that would normally be hard, and it is already there and
already tested — including the case that decides the order (`.card` and
`Button.success` naming the same value; the variant wins, as in CSS).

## What needs a convention, not an engine change

**Property names.** Ours are `"Normal Color"`, `"Hovered Color"`,
`"Corner Radius"` — they are an on-disk format and cannot be renamed. A CSS
front-end would accept the kebab form of the same name (`normal-color`,
`corner-radius`) and map it back. Not CSS's own vocabulary: `background-color`
means nothing to a Slider, and inventing a translation layer for 19 types is
work that buys a familiar spelling and costs an indirection nobody can debug.
The property list is generated from the types (`styleableProps` already does it
for the editor), so the mapping table writes itself.

**Pseudo-classes.** `Button:hover { background: … }` is a *rewrite*, not a
feature: `:hover` → the `"Hovered Color"` property of the same style,
`:active` → `"Pressed Color"`. It works exactly where the type has that
property, and the parser can say so by name when it does not. Worth doing — it
is the spelling people expect — but it is sugar over what exists.

**Roles and size steps.** The nine roles and the size/text steps are a second
vocabulary, and CSS has the right shape for it already: custom properties.
`--accent: #d08a2a;` at the top, `var(--accent)` in a declaration. That would
let the whole theme, roles included, be one file.

## What does not map, and the honest answer for each

**1. Light and dark are two values of one decision.** Every colour in a theme
carries both; CSS declarations carry one. Three options, in order of preference:

- `light-dark(#f4f4f4, #16171b)` — real CSS (Color Module 5), one declaration,
  both values, exactly our model. This is the one to take.
- an `@dark { … }` block that overrides the light values. Reads well, but it
  splits a decision the whole theme design exists to keep together.
- two files. No.

**2. Descendant and child combinators.** `Panel > Button { … }` is expressible
against the widget tree — `uiApplyTheme` walks it and every element knows its
parent — but it turns matching from "look up three keys" into "test every rule
against every element", and it drags in real specificity arithmetic. It is also
the feature that would make a theme unpredictable in the way this design has
avoided so far. Recommendation: leave it out of v1 and see whether anybody asks.

**3. Inheritance down the tree.** CSS inherits `color` and `font-size` from
parent to child; we inherit nothing through the theme (opacity, enabled and
visible are inherited by the *engine*, which is a different mechanism). Adding
it would mean a per-frame resolve instead of the assign-once model that makes
the runtime, the designer, the thumbnails and the software rasteriser all agree
by construction. That model is worth more than the feature. Leave out.

Also out, for the same "no rule an author cannot predict in their head" reason:
`!important`, `@media`, transitions and animations, shorthands
(`border: 1px solid red`), units other than px, and a bare `.tag` that matches
every type at once.

## What it would take

1. A tokeniser and parser for the subset above, in `HE_Core`, producing a
   `UITheme`. No dependency: the grammar is selectors, blocks, `name: value;`,
   and four value forms (hex colour, `rgb()`, `light-dark()`, number).
2. The reverse direction is free and worth having: `UITheme` → CSS is a
   30-line writer, since the model is a subset. That makes the CSS view a *view*
   rather than a second source of truth, and sidesteps the question of what
   happens to comments and ordering on a round trip.
3. In the editor: a "CSS" tab beside the Styles section that shows the writer's
   output and takes an edit through the parser. Errors reported with a line
   number, which the editor has no vocabulary for yet.
4. Tests: parse → write → parse must be a fixed point, and a corpus of files
   with deliberate errors that must be *reported* rather than silently dropped.

Estimate: the parser and writer are the small half. The editor's error surface
is the half that decides whether it is pleasant to use.
