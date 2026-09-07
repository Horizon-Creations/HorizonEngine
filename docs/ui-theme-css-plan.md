# Themes als CSS: Umsetzungsplan

Stand 07.09.2026, Branch `claude/he-apps-ui-framework-83b5f7`. **Kein Feature-Code
in diesem Schritt**, wie schon bei §13 von `docs/he-apps-plan.md`: hier steht, was
gebaut würde, in welcher Reihenfolge, und was vorher entschieden sein muss.

Bezug: `docs/ui-theme-css.md` (die Bewertung, „nothing here is built"),
`docs/he-apps-plan.md` D1 (das Theme-System selbst),
`src/HE_Core/include/UIWidget/UITheme.h`,
`src/HE_Core/src/UIWidget/UIWidgetTree.cpp:869` (die Kaskade),
`src/HE_Editor/ThemeAssetPanel.cpp` (der heutige Editor).

Alle „ist heute so"-Aussagen unten sind am Quelltext geprüft, mit Fundstelle.

---

## 1. Der eine Satz

**CSS ist eine ANSICHT auf ein Theme, kein zweiter Wahrheitsort.** Die Wahrheit
bleibt `ThemeAsset::json` (`Assets.h:385`), geschrieben von `uiThemeToJson`. Der
CSS-Text wird bei jedem Öffnen aus dem Theme erzeugt und beim Anwenden wieder in
dasselbe Modell zurückgelesen. Nichts in der Laufzeit, im Extractor, im
Software-Rasterizer oder im Packager erfährt, dass es CSS gibt.

Daraus folgt alles Weitere: Kommentare und die Zeilenreihenfolge des Autors
überleben ein Anwenden **nicht** (sie haben im Modell keinen Platz), und die
Frage „welche Datei gilt, wenn beide da sind" stellt sich nie.

---

## 2. Drei Korrekturen an der Bewertung

**2.1 Der verlustfreie Rundlauf ist die eigentliche Hürde, nicht der Parser.**
Farben liegen als `glm::vec4` (`UITheme.h`), und das mitgelieferte Default-Theme
ist von Hand auf zwei Nachkommastellen gesetzt: `0.43f` für MutedText hell
(`UITheme.cpp:169`), `0.96f`, `0.09f`, `0.35f` Schattenalpha. Ein Writer, der
`#rrggbbaa` ausgibt, quantisiert auf 8 Bit: `0.43` wird `#6e` wird `0.42745`.
Wer also den CSS-Reiter öffnet, **eine** Zeile ändert und anwendet, verschiebt
alle anderen Farben des Themes mit. Genau dort bricht das Versprechen aus §1
still. Zusätzlich hängen an den Werten Kontrastverhältnisse, die knapp gesetzt
wurden (`uiCheckContrast`, 4.30 gegen 4.5 in dem Kommentar bei `UITheme.cpp:163`).

Die Bewertung erwähnt das nicht. Die Schreibweise der Farben muss deshalb
verlustfrei sein, siehe §4.1.

**2.2 Der Editor kann Zeilennummern längst.** Die Bewertung sagt, Fehler mit
Zeilennummer seien etwas, wofür „the editor has no vocabulary yet". Das stimmt
nicht: `ScriptEditorPanel.cpp` benutzt das eingebundene ImGuiColorTextEdit mit
Zeilennummern, Undo und `SetViewAtLine`/`SelectLine`
(`src/HE_Editor/vendor/ImGuiColorTextEdit/TextEditor.h:61,78`). Was fehlt, ist
eine CSS-Sprachdefinition (die Aufzählung kennt nur None, Cpp, C, Cs, Python,
Lua, Json, Sql, AngelScript, Glsl, Hlsl) und eine öffentliche API für
Fehlermarken (die Palette hat `ErrorMarker`, ein Setter dafür fehlt). Beides ist
ein Vendor-Patch und gehört deshalb hinter die nutzbare Fassung, siehe CP4/CP4b.

*(Die Bewertung selbst wird nicht umgeschrieben, sie steht als Bewertung. Oben in
ihr steht jetzt nur ein Zeiger auf dieses Dokument.)*

**2.3 Schatten fehlen in der Abbildungstabelle.** Ein Theme hat zwei
`UIThemeShadow` (Raised, Overlay) mit Farbe, Blur und Versatz. CSS hat dafür die
passende Schreibweise schon: `box-shadow`-Syntax, `0 3px 8px <farbe>`. Kein
Sonderfall nötig, aber ein Feld, das sonst beim ersten Rundlauf verschwindet.

---

## 3. Was CSS abdeckt und was ausdrücklich nicht

Die Kaskade ist da und getestet (`UIWidgetTree.cpp:869`, Reihenfolge
Typ → Klasse → Typ.Variante, bewusst aus CSS). Der Parser erbt sie geschenkt.

**Was ein Theme ist, deckt CSS ab:** zehn Rollen mal zwei Modi, drei Radius- und
drei Spacing-Stufen, fünf Textgrößen, zwei Schatten, der Name, und die Styles
mit ihren Eigenschaften.

**Was ein Theme NICHT ist, deckt CSS auch nicht ab:** die Bindungen am Element.
`UIElement::themeRoles` (Eigenschaft → Rollenname), `themeStyle`, `themeTag` und
`kUIThemeLiteral` liegen im **Widget-Asset**, nicht im Theme. Das `class="card"`
aus der Tabelle der Bewertung ist `themeStyle` und wird im Designer gesetzt. Die
CSS-Ansicht hat also kein HTML daneben, und das ist kein Mangel, sondern die
Grenze: CSS beschreibt hier das Stylesheet, nicht das Dokument.

**Der Fund, der die v1 zuschneidet:** `UIThemeStyleValue` hat drei Felder,
`isColor`, `color[2]`, `number` (`UITheme.h`). Es gibt **keinen Zustand „dieser
Style-Wert folgt der Rolle Accent"**. `var(--accent)` innerhalb eines
Style-Blocks könnte der Parser also nur flachklopfen, und der Writer könnte
daraus nie wieder `var(--accent)` machen. Deshalb:

> **In v1 sind Custom Properties Definitionen, keine Referenzen.** `var()` in
> einer Style-Deklaration ist ein **Fehler mit Zeilennummer**, kein stilles
> Flachklopfen. Der Satz, den der Fehler sagt: „ein Style-Wert kann heute keiner
> Rolle folgen".

Die v2 dazu ist benannt, aber nicht Teil dieses Plans: ein Bindungsfeld an
`UIThemeStyleValue`, das dann `uiThemeToJson`/`FromJson`, `uiThemeValueFor` und
den Style-Editor mitnimmt.

---

## 4. Die Schreibweise, die vorher feststehen muss

### 4.1 Farben, verlustfrei

Regel: **Hex, wenn der Wert exakt in 8 Bit passt (`v*255` ganzzahlig), sonst
Prozent-`rgb()`.** `rgb(43% 43% 48% / 100%)` bildet die von Hand gesetzten
Zweikommastellen exakt ab, und Prozentwerte dürfen selbst Nachkommastellen haben.
Beide Formen liest der Parser, geschrieben wird immer die erste passende.

Beide Modi in einer Deklaration, per `light-dark(<hell>, <dunkel>)`. Das ist
echtes CSS (Color Module 5) und genau unser Modell, wie die Bewertung schon sagt.

Die Werte sind Anzeigewerte (sRGB), so wie sie heute in der Datei und im
`ColorEdit4` des Theme-Panels stehen. Es wird nichts umgerechnet.

### 4.2 Ein `:root`-Block für alles, was keine Style-Eigenschaft ist

Ein nacktes `--accent: …;` ohne Block ist kein CSS. Also:

```css
:root {
  --name: "Amber";
  --accent: light-dark(#d08a2a, rgb(88% 62% 26%));
  --radius-medium: 8px;
  --spacing-small: 4px;
  --text-body: 16px;
  --shadow-raised: 0 3px 8px light-dark(rgb(0% 0% 0% / 35%), rgb(0% 0% 0% / 55%));
}
```

Damit bekommt auch der Theme-Name einen rundlauffähigen Platz, ohne dass ein
At-Rule erfunden werden muss.

### 4.3 Eigenschaftsnamen: gefaltet, Selektoren: wörtlich

Eigenschaftsnamen sind ein On-Disk-Format und werden nicht umbenannt (der Kasten
oben in `UITheme.h`). Der CSS-Blick zeigt ihre Kebab-Form: `"Normal Color"` wird
`normal-color`, `"FontSize"` wird `font-size`, `"Corner Radius"` wird
`corner-radius`. Keine CSS-Vokabeln, aus dem Grund, den die Bewertung nennt.

Nachgezählt: über alle Typen gibt es heute **73 verschiedene Color- und
Float-Eigenschaften, und die Faltung ist auf ihnen eineindeutig** (147 über alle
Typen hinweg, ebenfalls kollisionsfrei). Bewacht ist das nicht: eine künftige
Eigenschaft „Font Size" neben „FontSize" würde beide auf `font-size` legen und
den Rundlauf still verbiegen. Das ist der Grund, warum CP0 zuerst kommt.

**Selektoren dagegen werden wörtlich und schreibungsgetreu geschrieben**
(`Button`, `Card`, `Button.success`), weil sie Schlüssel im Modell sind. Ein Tag
oder ein freier Name mit Leerzeichen oder Punkt ist als Selektor nicht
ausdrückbar; der Writer muss das melden statt etwas Halbes zu schreiben.

### 4.4 Zahlen

Geschrieben wird `8px`, gelesen werden `8` und `8px`. Andere Einheiten sind ein
Fehler mit Zeilennummer, keine Umrechnung.

---

## 5. Die Reihenfolge

| CP | Was | Wo | Größe |
|---|---|---|---|
| **CP0** | Die Faltung und ihr Wächter | `HE_Core` | S |
| **CP1** | Writer `uiThemeToCss` | `HE_Core` | M |
| **CP2** | Parser `uiThemeFromCss` mit Diagnosen | `HE_Core` | L |
| **CP3** | Feinschliff der Schreibweise | `HE_Core` | S |
| **CP4** | Der CSS-Reiter im Theme-Editor | `HE_Editor` | M |
| **CP4b** | Hervorhebung und Fehlermarken | Vendor-Patch | S |
| **CP5** | Datei rein, Datei raus | `HE_Editor` | S |

**CP0 — die Faltung und ihr Wächter.** `uiThemeCssPropName(name)` und die
Rückrichtung, neue Datei `src/HE_Core/src/UIWidget/UIThemeCss.cpp` mit Header
daneben, `HE_API` an jedem Symbol (Windows-Regel für HE_Core). Der Test läuft
über die ganze Typregistrierung (`uiWidgetTypeRegistry` plus
`uiBaseProperties()` und `makeUIElement(t)->properties()`, alle in HE_Core, der
Helfer `styleableProps` im Editor ist nur ein Zwölfzeiler darüber) und prüft:
jede Color- und Float-Eigenschaft faltet eindeutig, und die Rückrichtung trifft
wieder denselben Namen. Bricht der Test, ist eine neue Eigenschaft schuld und
nicht der Parser. Ohne Feature-Wert, aber es ist die Zusicherung, auf der CP1 und
CP2 stehen.

**CP1 — Writer.** `std::string uiThemeToCss(const UITheme&)`. Deterministisch und
in der Reihenfolge des Modells (die Styles sind ein `vector`, kein `map`, genau
deswegen). Zwei Tests:

1. **Der Rundlauf, der zählt:** `uiThemeFromJson → uiThemeToCss → uiThemeFromCss
   → uiThemeToJson` ist **textgleich** zum Ausgangs-JSON, für Default und Amber.
   Nicht bloß Parse-Write-Parse: die JSON-Seite ist die, an der die
   Quantisierung aus §2.1 auffliegt. (Dieser Test wird erst mit CP2 grün, er
   gehört aber hierher geschrieben.)
2. **Nichts fällt still weg:** jede Rolle, jede Stufe, jeder Schatten, jeder
   Style-Eintrag taucht im Text auf.

**CP2 — Parser.** Tokenizer und Parser für die Teilmenge, ohne neue Abhängigkeit:
Selektoren, Blöcke, `name: wert;`, dazu Hex, `rgb()`, `light-dark()`, Zahl mit
optionalem `px` und String. Ergebnis ist ein `UITheme` **und eine Liste von
Diagnosen** (Zeile, Spalte, Satz), nie eine stille Teilübernahme: entweder das
Theme ist gültig, oder es wird nichts angewendet. Testkorpus: eine Sammlung
absichtlich kaputter Dateien, bei denen jede genau **eine** benannte Diagnose
erzeugen muss, plus der Rundlauf aus CP1.

**CP3 — Feinschliff.** Zwei Kleinigkeiten, die einzeln nichts blockieren:
`var(--rolle)` als Referenz **nur** dort, wo es rundlauffähig ist (also
innerhalb von `:root`, etwa eine Stufe, die auf eine andere zeigt), und die
Fehlermeldung für den Style-Fall aus §3. Pseudoklassen sind hier bewusst
**nicht** drin, siehe §6.

**CP4 — der Reiter.** Neben „Styles" im `ThemeAssetPanel` ein Reiter „CSS":
TextEditor mit `LanguageDefinitionId::None`, darunter die Diagnoseliste,
Klick auf eine Zeile springt per `SetViewAtLine` hin. Zwei Knöpfe, „Aus Theme
erzeugen" und „Anwenden". Kein Vendor-Patch. Der Help-Scope kommt dazu, sonst
schlägt die Deckungsprüfung zu (`docs/in-engine-docs-design.md`).

**CP4b — Hervorhebung.** CSS-Sprachdefinition und eine Setter-API für
Fehlermarken im eingebundenen TextEditor, als `HE-PATCH(css-lang)` markiert, wie
die Patches an imgui und ImGuizmo, damit ein Re-Vendoring sie nicht verschluckt.
Rein kosmetisch, deshalb hinten.

**CP5 — Datei.** Export in eine `.css` und Import daraus, über den Dateidialog.
Bewusst **kein** neuer Assettyp: eine `.css` im Projekt wäre der zweite
Wahrheitsort aus §1.

---

## 6. Was draußen bleibt

Alles aus der Bewertung: Nachfahren- und Kind-Kombinatoren, Vererbung über den
Baum, `!important`, `@media`, Transitions, Kurzformen, andere Einheiten als px,
und ein nacktes `.tag`, das jeden Typ trifft.

**Neu dazu, gegen die Bewertung:**

- **Pseudoklassen (`:hover`, `:active`) in v1 nicht.** Die Bewertung nennt sie
  „sugar over what exists", legt aber nicht fest, was **in** den Klammern steht:
  wird `normal-color` zu „Hovered Color" umgeschrieben, oder heißt es dort
  `color`? Solange das nicht entschieden ist, verletzt es die Hausregel „keine
  Regel, die ein Autor nicht im Kopf vorhersagen kann". Und es wird nicht
  gebraucht: `hovered-color:` ist bereits eindeutig und kostet nichts.
- **`var()` in Style-Blöcken** (§3), bis das Modell eine Bindung kennt.
- **Kommentare und Autorenreihenfolge über ein Anwenden hinweg** (§1).

---

## 7. Risiken, in der Reihenfolge, in der sie beißen

1. **Quantisierung** (§2.1). Der Rundlauftest aus CP1 ist die einzige
   Absicherung; wird er weggelassen, merkt es niemand, bis ein Theme langsam
   driftet.
2. **`Color` heißt in CSS etwas anderes.** Die Flächenfarbe eines Panels heißt
   `"Color"` und wird zu `color:`, was ein CSS-Leser als Textfarbe liest. Wird
   in Kauf genommen, aus dem Grund der Bewertung: eine Übersetzungsschicht für
   19 Typen kauft eine vertraute Schreibweise und kostet eine Indirektion, die
   niemand debuggen kann.
3. **Die Faltung ist heute eineindeutig, aber unbewacht** (§4.3). CP0.
4. **Die Fehleroberfläche ist die teure Hälfte**, nicht der Parser. Das sagt die
   Bewertung schon, und CP4 ist deshalb absichtlich vor CP4b geschnitten: erst
   benutzbar, dann hübsch.

---

## 8. Was der Mensch entscheiden muss, bevor jemand anfängt

1. **Ob überhaupt.** Der Nutzen ist eine vertraute Schreibweise und ein Theme,
   das sich in einem Diff lesen lässt. Das Theme-Panel kann heute alles davon,
   nur eben nicht als Text.
2. **Wie weit.** CP0 bis CP2 ohne CP4 ergibt schon etwas Nutzbares für Skripte
   und Werkzeuge, aber nichts, was man im Editor sieht. CP0 bis CP4 ist die
   kleinste Fassung, die ein Autor bemerkt.
3. **Ob der `var()`-Fall (§3) v1 offen bleibt** oder ob gleich das Bindungsfeld
   an `UIThemeStyleValue` gebaut wird. Das ist die einzige Stelle, an der CSS
   das Modell ändern wollen würde.
