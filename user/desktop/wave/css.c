/*
 * CSS: style sheet parsing, selector matching and the cascade.
 *
 * A sheet is a list of rules (selector list + declarations). @media
 * blocks are kept when their query matches a screen of the viewport's
 * size in light mode, @supports blocks when this file can do what they
 * ask; nested rules (CSS nesting) become rules of their own with the
 * parent's selector put in for &. Other at-rules are skipped. Selectors support
 * type, class, id, attribute and the structural pseudo-classes, with
 * all four combinators; :hover and the like never match, and rules for
 * pseudo-elements are dropped.
 *
 * Each element's declarations are sorted by origin and importance, then
 * specificity, then order, and applied over the inherited style: custom
 * properties first (so var() can use them), then font-size (so em units
 * can), then the rest.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "web.h"

#define MAX_PARTS   16
#define MAX_MATCHES 512
#define INDEX_BUCKETS 256

enum prop {
    P_UNKNOWN, P_CUSTOM,
    P_DISPLAY, P_COLOR, P_BACKGROUND, P_BACKGROUND_COLOR, P_FONT, P_FONT_SIZE, P_FONT_WEIGHT,
    P_FONT_STYLE, P_FONT_FAMILY, P_LINE_HEIGHT, P_TEXT_ALIGN, P_TEXT_DECORATION,
    P_TEXT_DECORATION_LINE, P_TEXT_TRANSFORM, P_TEXT_INDENT, P_WHITE_SPACE, P_VERTICAL_ALIGN,
    P_LIST_STYLE, P_LIST_STYLE_TYPE, P_MARGIN, P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM,
    P_MARGIN_LEFT, P_PADDING, P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_BORDER, P_BORDER_TOP, P_BORDER_RIGHT, P_BORDER_BOTTOM, P_BORDER_LEFT, P_BORDER_WIDTH,
    P_BORDER_STYLE, P_BORDER_COLOR, P_BORDER_TOP_WIDTH, P_BORDER_RIGHT_WIDTH,
    P_BORDER_BOTTOM_WIDTH, P_BORDER_LEFT_WIDTH, P_BORDER_TOP_STYLE, P_BORDER_RIGHT_STYLE,
    P_BORDER_BOTTOM_STYLE, P_BORDER_LEFT_STYLE, P_BORDER_TOP_COLOR, P_BORDER_RIGHT_COLOR,
    P_BORDER_BOTTOM_COLOR, P_BORDER_LEFT_COLOR, P_WIDTH, P_HEIGHT, P_MIN_WIDTH, P_MAX_WIDTH,
    P_MIN_HEIGHT, P_BOX_SIZING, P_VISIBILITY, P_FLOAT, P_FLEX_DIRECTION, P_FLEX_FLOW,
    P_BORDER_SPACING, P_BORDER_COLLAPSE, P_POSITION, P_OPACITY, P_LEFT, P_TOP, P_CLIP, P_MASK,
    P_CONTENT, P_BACKGROUND_IMAGE, P_BORDER_RADIUS, P_BORDER_TL_RADIUS, P_BORDER_TR_RADIUS,
    P_BORDER_BR_RADIUS, P_BORDER_BL_RADIUS, P_FLEX, P_FLEX_GROW, P_FLEX_SHRINK, P_FLEX_BASIS,
    P_FLEX_WRAP, P_JUSTIFY_CONTENT, P_ALIGN_ITEMS, P_ALIGN_SELF, P_JUSTIFY_ITEMS, P_JUSTIFY_SELF,
    P_PLACE_CONTENT, P_PLACE_ITEMS, P_PLACE_SELF, P_ORDER, P_GAP, P_ROW_GAP, P_COLUMN_GAP,
    P_OVERFLOW, P_GRID_TEMPLATE_COLUMNS, P_GRID_TEMPLATE, P_GRID_COLUMN, P_GRID_COLUMN_START,
    P_GRID_COLUMN_END, P_GRID_ROW, P_GRID_ROW_START, P_GRID_AREA, P_GRID_TEMPLATE_ROWS,
    P_GRID_TEMPLATE_AREAS, P_GRID_AUTO_ROWS, P_GRID_ROW_END, P_GRID_AUTO_FLOW, P_MAX_HEIGHT,
    P_ASPECT_RATIO, P_RIGHT, P_BOTTOM, P_CLEAR, P_INSET, P_TRANSFORM, P_TRANSFORM_ORIGIN, P_PERSPECTIVE, P_PERSPECTIVE_ORIGIN, P_Z_INDEX, P_BOX_SHADOW,
    P_MARGIN_INLINE, P_MARGIN_BLOCK, P_PADDING_INLINE, P_PADDING_BLOCK, P_INSET_INLINE, P_INSET_BLOCK,
    P_CURSOR, P_POINTER_EVENTS, P_USER_SELECT, P_IGNORED, P_LETTER_SPACING, P_WORD_SPACING,
    P_WORD_BREAK, P_OVERFLOW_WRAP, P_TEXT_OVERFLOW, P_OUTLINE, P_OUTLINE_STYLE, P_OUTLINE_COLOR,
    P_OUTLINE_WIDTH, P_OUTLINE_OFFSET, P_TEXT_DECORATION_COLOR, P_TEXT_DECORATION_STYLE,
    P_FONT_VARIANT, P_LIST_STYLE_POSITION, P_TABLE_LAYOUT, P_CAPTION_SIDE, P_EMPTY_CELLS,
    P_TEXT_SHADOW, P_BACKGROUND_SIZE, P_BACKGROUND_POSITION, P_BACKGROUND_POSITION_X,
    P_BACKGROUND_POSITION_Y, P_BACKGROUND_REPEAT, P_BACKGROUND_ORIGIN, P_OBJECT_FIT,
    P_OBJECT_POSITION, P_OVERFLOW_X, P_OVERFLOW_Y, P_TAB_SIZE, P_TEXT_ALIGN_LAST, P_LINE_CLAMP,
    P_LIST_STYLE_IMAGE, P_COUNTER_RESET, P_COUNTER_INCREMENT,
    P_DECO_THICKNESS, P_UNDERLINE_OFFSET, P_BACKGROUND_CLIP, P_BACKGROUND_ATTACHMENT,
    P_ALL, P_ALIGN_CONTENT, P_GRID_AUTO_COLUMNS, P_TRANSLATE, P_ROTATE, P_SCALE, P_TEXT_WRAP,
    P_TRANSITION, P_TRANSITION_PROPERTY, P_TRANSITION_DURATION, P_TRANSITION_TIMING, P_TRANSITION_DELAY,
    P_ANIMATION, P_ANIMATION_NAME, P_ANIMATION_DURATION, P_ANIMATION_TIMING, P_ANIMATION_DELAY,
    P_ANIMATION_ITERATION, P_ANIMATION_DIRECTION, P_ANIMATION_FILL, P_ANIMATION_STATE,
    P_COLUMNS, P_COLUMN_COUNT, P_COLUMN_WIDTH, P_COLUMN_RULE, P_COLUMN_RULE_WIDTH, P_COLUMN_RULE_STYLE,
    P_COLUMN_RULE_COLOR, P_COLUMN_FILL, P_BREAK_INSIDE, P_BREAK_BEFORE, P_BREAK_AFTER,
    P_CONTAINER, P_CONTAINER_NAME, P_CONTAINER_TYPE, P_BACKFACE, P_FILTER, P_BACKDROP, P_CLIP_PATH, P_BLEND,
    P_TRANSITION_BEHAVIOR, P_ANCHOR_NAME, P_POSITION_ANCHOR, P_POSITION_AREA, P_POSITION_TRY_FALLBACKS,
    P_POSITION_TRY,
    P_COUNT
};
_Static_assert(P_COUNT <= 256, "a declaration keeps its property in a byte");
_Static_assert(AP_COUNT <= 64, "transition-property's names are bits of a 64-bit mask");

/* Transitions and animations, with or without -webkit- (looked up before prop_names). */
static const struct { const char *name; uint8_t prop; } motion_names[] = {
    { "animation", P_ANIMATION }, { "animation-composition", P_IGNORED },
    { "animation-delay", P_ANIMATION_DELAY }, { "animation-direction", P_ANIMATION_DIRECTION },
    { "animation-duration", P_ANIMATION_DURATION }, { "animation-fill-mode", P_ANIMATION_FILL },
    { "animation-iteration-count", P_ANIMATION_ITERATION }, { "animation-name", P_ANIMATION_NAME },
    { "animation-play-state", P_ANIMATION_STATE }, { "animation-range", P_IGNORED },
    { "animation-timeline", P_IGNORED }, { "animation-timing-function", P_ANIMATION_TIMING },
    { "transition", P_TRANSITION }, { "transition-behavior", P_TRANSITION_BEHAVIOR },
    { "transition-delay", P_TRANSITION_DELAY }, { "transition-duration", P_TRANSITION_DURATION },
    { "transition-property", P_TRANSITION_PROPERTY }, { "transition-timing-function", P_TRANSITION_TIMING },
};

static const struct { const char *name; uint8_t prop; } prop_names[] = {
    { "-moz-appearance", P_IGNORED }, { "-moz-font-feature-settings", P_IGNORED },
    { "-moz-osx-font-smoothing", P_IGNORED }, { "-moz-text-size-adjust", P_IGNORED },
    { "-moz-user-select", P_USER_SELECT }, { "-ms-flex", P_FLEX }, { "-ms-overflow-style", P_IGNORED },
    { "-ms-text-size-adjust", P_IGNORED }, { "-ms-user-select", P_USER_SELECT },
    { "-webkit-align-content", P_ALIGN_CONTENT }, { "-webkit-align-items", P_ALIGN_ITEMS },
    { "-webkit-align-self", P_ALIGN_SELF }, { "-webkit-appearance", P_IGNORED }, { "-webkit-backdrop-filter", P_BACKDROP },
    { "-webkit-backface-visibility", P_BACKFACE }, { "-webkit-border-radius", P_BORDER_RADIUS },
    { "-webkit-box-decoration-break", P_IGNORED }, { "-webkit-box-orient", P_IGNORED },
    { "-webkit-box-shadow", P_BOX_SHADOW }, { "-webkit-box-sizing", P_BOX_SIZING }, { "-webkit-clip-path", P_CLIP_PATH },
    { "-webkit-column-break-after", P_BREAK_AFTER }, { "-webkit-column-break-before", P_BREAK_BEFORE },
    { "-webkit-column-break-inside", P_BREAK_INSIDE }, { "-webkit-column-count", P_COLUMN_COUNT },
    { "-webkit-column-gap", P_COLUMN_GAP }, { "-webkit-column-rule", P_COLUMN_RULE },
    { "-webkit-column-width", P_COLUMN_WIDTH }, { "-webkit-columns", P_COLUMNS }, { "-webkit-filter", P_FILTER }, { "-webkit-flex", P_FLEX },
    { "-webkit-flex-basis", P_FLEX_BASIS }, { "-webkit-flex-direction", P_FLEX_DIRECTION },
    { "-webkit-flex-flow", P_FLEX_FLOW }, { "-webkit-flex-grow", P_FLEX_GROW },
    { "-webkit-flex-shrink", P_FLEX_SHRINK }, { "-webkit-flex-wrap", P_FLEX_WRAP },
    { "-webkit-font-feature-settings", P_IGNORED }, { "-webkit-font-smoothing", P_IGNORED },
    { "-webkit-hyphens", P_IGNORED }, { "-webkit-justify-content", P_JUSTIFY_CONTENT },
    { "-webkit-line-clamp", P_LINE_CLAMP }, { "-webkit-mask", P_MASK }, { "-webkit-mask-image", P_MASK },
    { "-webkit-order", P_ORDER }, { "-webkit-overflow-scrolling", P_IGNORED },
    { "-webkit-print-color-adjust", P_IGNORED }, { "-webkit-tap-highlight-color", P_IGNORED },
    { "-webkit-text-size-adjust", P_IGNORED }, { "-webkit-touch-callout", P_IGNORED },
    { "-webkit-transform", P_TRANSFORM }, { "-webkit-transform-origin", P_TRANSFORM_ORIGIN },
    { "-webkit-user-drag", P_IGNORED }, { "-webkit-user-select", P_USER_SELECT }, { "accent-color", P_IGNORED },
    { "align-content", P_ALIGN_CONTENT }, { "align-items", P_ALIGN_ITEMS }, { "align-self", P_ALIGN_SELF },
    { "alignment-baseline", P_IGNORED }, { "all", P_ALL }, { "anchor-name", P_ANCHOR_NAME }, { "anchor-scope", P_IGNORED }, { "appearance", P_IGNORED },
    { "aspect-ratio", P_ASPECT_RATIO }, { "backdrop-filter", P_BACKDROP }, { "backface-visibility", P_BACKFACE }, { "background", P_BACKGROUND },
    { "background-attachment", P_BACKGROUND_ATTACHMENT }, { "background-clip", P_BACKGROUND_CLIP },
    { "background-color", P_BACKGROUND_COLOR }, { "background-image", P_BACKGROUND_IMAGE },
    { "background-origin", P_BACKGROUND_ORIGIN }, { "background-position", P_BACKGROUND_POSITION },
    { "background-position-x", P_BACKGROUND_POSITION_X }, { "background-position-y", P_BACKGROUND_POSITION_Y },
    { "background-repeat", P_BACKGROUND_REPEAT }, { "background-size", P_BACKGROUND_SIZE },
    { "baseline-shift", P_IGNORED }, { "block-size", P_HEIGHT }, { "border", P_BORDER },
    { "border-block-end", P_BORDER_BOTTOM }, { "border-block-end-color", P_BORDER_BOTTOM_COLOR },
    { "border-block-end-width", P_BORDER_BOTTOM_WIDTH }, { "border-block-start", P_BORDER_TOP },
    { "border-block-start-color", P_BORDER_TOP_COLOR }, { "border-block-start-width", P_BORDER_TOP_WIDTH },
    { "border-bottom", P_BORDER_BOTTOM }, { "border-bottom-color", P_BORDER_BOTTOM_COLOR },
    { "border-bottom-left-radius", P_BORDER_BL_RADIUS }, { "border-bottom-right-radius", P_BORDER_BR_RADIUS },
    { "border-bottom-style", P_BORDER_BOTTOM_STYLE }, { "border-bottom-width", P_BORDER_BOTTOM_WIDTH },
    { "border-collapse", P_BORDER_COLLAPSE }, { "border-color", P_BORDER_COLOR },
    { "border-end-end-radius", P_BORDER_BR_RADIUS }, { "border-end-start-radius", P_BORDER_BL_RADIUS },
    { "border-inline-end", P_BORDER_RIGHT }, { "border-inline-end-color", P_BORDER_RIGHT_COLOR },
    { "border-inline-end-width", P_BORDER_RIGHT_WIDTH }, { "border-inline-start", P_BORDER_LEFT },
    { "border-inline-start-color", P_BORDER_LEFT_COLOR }, { "border-inline-start-width", P_BORDER_LEFT_WIDTH },
    { "border-left", P_BORDER_LEFT }, { "border-left-color", P_BORDER_LEFT_COLOR },
    { "border-left-style", P_BORDER_LEFT_STYLE }, { "border-left-width", P_BORDER_LEFT_WIDTH },
    { "border-radius", P_BORDER_RADIUS }, { "border-right", P_BORDER_RIGHT },
    { "border-right-color", P_BORDER_RIGHT_COLOR }, { "border-right-style", P_BORDER_RIGHT_STYLE },
    { "border-right-width", P_BORDER_RIGHT_WIDTH }, { "border-spacing", P_BORDER_SPACING },
    { "border-start-end-radius", P_BORDER_TR_RADIUS }, { "border-start-start-radius", P_BORDER_TL_RADIUS },
    { "border-style", P_BORDER_STYLE }, { "border-top", P_BORDER_TOP },
    { "border-top-color", P_BORDER_TOP_COLOR }, { "border-top-left-radius", P_BORDER_TL_RADIUS },
    { "border-top-right-radius", P_BORDER_TR_RADIUS }, { "border-top-style", P_BORDER_TOP_STYLE },
    { "border-top-width", P_BORDER_TOP_WIDTH }, { "border-width", P_BORDER_WIDTH }, { "bottom", P_BOTTOM },
    { "box-decoration-break", P_IGNORED }, { "box-shadow", P_BOX_SHADOW }, { "box-sizing", P_BOX_SIZING },
    { "break-after", P_BREAK_AFTER }, { "break-before", P_BREAK_BEFORE }, { "break-inside", P_BREAK_INSIDE },
    { "caption-side", P_CAPTION_SIDE }, { "caret-color", P_IGNORED }, { "clear", P_CLEAR }, { "clip", P_CLIP }, { "clip-path", P_CLIP_PATH },
    { "color", P_COLOR }, { "color-interpolation", P_IGNORED }, { "color-interpolation-filters", P_IGNORED },
    { "color-rendering", P_IGNORED }, { "color-scheme", P_IGNORED }, { "column-count", P_COLUMN_COUNT },
    { "column-fill", P_COLUMN_FILL }, { "column-gap", P_COLUMN_GAP }, { "column-rule", P_COLUMN_RULE },
    { "column-rule-color", P_COLUMN_RULE_COLOR }, { "column-rule-style", P_COLUMN_RULE_STYLE },
    { "column-rule-width", P_COLUMN_RULE_WIDTH }, { "column-span", P_IGNORED }, { "column-width", P_COLUMN_WIDTH },
    { "columns", P_COLUMNS },
    { "contain", P_IGNORED }, { "contain-intrinsic-block-size", P_IGNORED },
    { "contain-intrinsic-height", P_IGNORED }, { "contain-intrinsic-inline-size", P_IGNORED },
    { "contain-intrinsic-size", P_IGNORED }, { "contain-intrinsic-width", P_IGNORED },
    { "container", P_CONTAINER }, { "container-name", P_CONTAINER_NAME }, { "container-type", P_CONTAINER_TYPE },
    { "content", P_CONTENT }, { "content-visibility", P_IGNORED }, { "counter-increment", P_COUNTER_INCREMENT },
    { "counter-reset", P_COUNTER_RESET }, { "counter-set", P_IGNORED }, { "cursor", P_CURSOR },
    { "display", P_DISPLAY }, { "dominant-baseline", P_IGNORED }, { "dynamic-range-limit", P_IGNORED },
    { "empty-cells", P_EMPTY_CELLS }, { "field-sizing", P_IGNORED }, { "filter", P_FILTER }, { "flex", P_FLEX },
    { "flex-basis", P_FLEX_BASIS }, { "flex-direction", P_FLEX_DIRECTION }, { "flex-flow", P_FLEX_FLOW },
    { "flex-grow", P_FLEX_GROW }, { "flex-shrink", P_FLEX_SHRINK }, { "flex-wrap", P_FLEX_WRAP },
    { "float", P_FLOAT }, { "font", P_FONT }, { "font-display", P_IGNORED }, { "font-family", P_FONT_FAMILY },
    { "font-feature-settings", P_IGNORED }, { "font-kerning", P_IGNORED },
    { "font-language-override", P_IGNORED }, { "font-optical-sizing", P_IGNORED },
    { "font-palette", P_IGNORED }, { "font-size", P_FONT_SIZE }, { "font-size-adjust", P_IGNORED },
    { "font-smooth", P_IGNORED }, { "font-style", P_FONT_STYLE }, { "font-synthesis", P_IGNORED },
    { "font-synthesis-small-caps", P_IGNORED }, { "font-synthesis-style", P_IGNORED },
    { "font-synthesis-weight", P_IGNORED }, { "font-variant", P_FONT_VARIANT },
    { "font-variant-alternates", P_IGNORED }, { "font-variant-caps", P_FONT_VARIANT },
    { "font-variant-east-asian", P_IGNORED }, { "font-variant-ligatures", P_IGNORED },
    { "font-variant-numeric", P_IGNORED }, { "font-variant-position", P_IGNORED },
    { "font-variation-settings", P_IGNORED }, { "font-weight", P_FONT_WEIGHT },
    { "forced-color-adjust", P_IGNORED }, { "gap", P_GAP }, { "grid-area", P_GRID_AREA },
    { "grid-auto-columns", P_GRID_AUTO_COLUMNS }, { "grid-auto-flow", P_GRID_AUTO_FLOW },
    { "grid-auto-rows", P_GRID_AUTO_ROWS }, { "grid-column", P_GRID_COLUMN },
    { "grid-column-end", P_GRID_COLUMN_END }, { "grid-column-gap", P_COLUMN_GAP },
    { "grid-column-start", P_GRID_COLUMN_START }, { "grid-gap", P_GAP }, { "grid-row", P_GRID_ROW },
    { "grid-row-end", P_GRID_ROW_END }, { "grid-row-gap", P_ROW_GAP }, { "grid-row-start", P_GRID_ROW_START },
    { "grid-template", P_GRID_TEMPLATE }, { "grid-template-areas", P_GRID_TEMPLATE_AREAS },
    { "grid-template-columns", P_GRID_TEMPLATE_COLUMNS }, { "grid-template-rows", P_GRID_TEMPLATE_ROWS },
    { "hanging-punctuation", P_IGNORED }, { "height", P_HEIGHT }, { "hyphens", P_IGNORED },
    { "image-rendering", P_IGNORED }, { "inline-size", P_WIDTH }, { "inset", P_INSET },
    { "inset-area", P_POSITION_AREA }, { "inset-block", P_INSET_BLOCK }, { "inset-block-end", P_BOTTOM }, { "inset-block-start", P_TOP },
    { "inset-inline", P_INSET_INLINE }, { "inset-inline-end", P_RIGHT }, { "inset-inline-start", P_LEFT },
    { "interpolate-size", P_IGNORED }, { "isolation", P_IGNORED }, { "justify-content", P_JUSTIFY_CONTENT },
    { "justify-items", P_JUSTIFY_ITEMS }, { "justify-self", P_JUSTIFY_SELF }, { "left", P_LEFT },
    { "letter-spacing", P_LETTER_SPACING }, { "line-break", P_IGNORED }, { "line-clamp", P_LINE_CLAMP },
    { "line-height", P_LINE_HEIGHT }, { "list-style", P_LIST_STYLE },
    { "list-style-image", P_LIST_STYLE_IMAGE }, { "list-style-position", P_LIST_STYLE_POSITION },
    { "list-style-type", P_LIST_STYLE_TYPE }, { "margin", P_MARGIN }, { "margin-block", P_MARGIN_BLOCK },
    { "margin-block-end", P_MARGIN_BOTTOM }, { "margin-block-start", P_MARGIN_TOP },
    { "margin-bottom", P_MARGIN_BOTTOM }, { "margin-inline", P_MARGIN_INLINE },
    { "margin-inline-end", P_MARGIN_RIGHT }, { "margin-inline-start", P_MARGIN_LEFT },
    { "margin-left", P_MARGIN_LEFT }, { "margin-right", P_MARGIN_RIGHT }, { "margin-top", P_MARGIN_TOP },
    { "margin-trim", P_IGNORED }, { "mask", P_MASK }, { "mask-image", P_MASK }, { "mask-type", P_IGNORED },
    { "math-depth", P_IGNORED }, { "math-style", P_IGNORED }, { "max-block-size", P_MAX_HEIGHT },
    { "max-height", P_MAX_HEIGHT }, { "max-inline-size", P_MAX_WIDTH }, { "max-width", P_MAX_WIDTH },
    { "min-block-size", P_MIN_HEIGHT }, { "min-height", P_MIN_HEIGHT }, { "min-inline-size", P_MIN_WIDTH },
    { "min-width", P_MIN_WIDTH }, { "mix-blend-mode", P_BLEND }, { "object-fit", P_OBJECT_FIT }, { "object-position", P_OBJECT_POSITION },
    { "opacity", P_OPACITY }, { "order", P_ORDER }, { "orphans", P_IGNORED }, { "outline", P_OUTLINE },
    { "outline-color", P_OUTLINE_COLOR }, { "outline-offset", P_OUTLINE_OFFSET },
    { "outline-style", P_OUTLINE_STYLE }, { "outline-width", P_OUTLINE_WIDTH }, { "overflow", P_OVERFLOW },
    { "overflow-anchor", P_IGNORED }, { "overflow-clip-margin", P_IGNORED },
    { "overflow-wrap", P_OVERFLOW_WRAP }, { "overflow-x", P_OVERFLOW_X }, { "overflow-y", P_OVERFLOW_Y },
    { "overscroll-behavior", P_IGNORED }, { "overscroll-behavior-block", P_IGNORED },
    { "overscroll-behavior-inline", P_IGNORED }, { "overscroll-behavior-x", P_IGNORED },
    { "overscroll-behavior-y", P_IGNORED }, { "padding", P_PADDING }, { "padding-block", P_PADDING_BLOCK },
    { "padding-block-end", P_PADDING_BOTTOM }, { "padding-block-start", P_PADDING_TOP },
    { "padding-bottom", P_PADDING_BOTTOM }, { "padding-inline", P_PADDING_INLINE },
    { "padding-inline-end", P_PADDING_RIGHT }, { "padding-inline-start", P_PADDING_LEFT },
    { "padding-left", P_PADDING_LEFT }, { "padding-right", P_PADDING_RIGHT }, { "padding-top", P_PADDING_TOP },
    { "page-break-after", P_IGNORED }, { "page-break-before", P_IGNORED }, { "page-break-inside", P_BREAK_INSIDE },
    { "paint-order", P_IGNORED }, { "perspective", P_PERSPECTIVE }, { "perspective-origin", P_PERSPECTIVE_ORIGIN },
    { "place-content", P_PLACE_CONTENT }, { "place-items", P_PLACE_ITEMS }, { "place-self", P_PLACE_SELF },
    { "pointer-events", P_POINTER_EVENTS }, { "position", P_POSITION }, { "position-anchor", P_POSITION_ANCHOR }, { "position-area", P_POSITION_AREA }, { "position-try", P_POSITION_TRY }, { "position-try-fallbacks", P_POSITION_TRY_FALLBACKS }, { "position-try-options", P_POSITION_TRY_FALLBACKS }, { "position-try-order", P_IGNORED }, { "position-visibility", P_IGNORED }, { "print-color-adjust", P_IGNORED },
    { "quotes", P_IGNORED }, { "resize", P_IGNORED }, { "right", P_RIGHT }, { "rotate", P_ROTATE },
    { "row-gap", P_ROW_GAP }, { "ruby-align", P_IGNORED }, { "ruby-position", P_IGNORED }, { "scale", P_SCALE },
    { "scroll-behavior", P_IGNORED }, { "scroll-margin", P_IGNORED }, { "scroll-margin-block", P_IGNORED },
    { "scroll-margin-bottom", P_IGNORED }, { "scroll-margin-inline", P_IGNORED },
    { "scroll-margin-left", P_IGNORED }, { "scroll-margin-right", P_IGNORED },
    { "scroll-margin-top", P_IGNORED }, { "scroll-padding", P_IGNORED }, { "scroll-padding-block", P_IGNORED },
    { "scroll-padding-bottom", P_IGNORED }, { "scroll-padding-inline", P_IGNORED },
    { "scroll-padding-left", P_IGNORED }, { "scroll-padding-right", P_IGNORED },
    { "scroll-padding-top", P_IGNORED }, { "scroll-snap-align", P_IGNORED }, { "scroll-snap-stop", P_IGNORED },
    { "scroll-snap-type", P_IGNORED }, { "scrollbar-color", P_IGNORED }, { "scrollbar-gutter", P_IGNORED },
    { "scrollbar-width", P_IGNORED }, { "shape-rendering", P_IGNORED }, { "speak", P_IGNORED },
    { "speak-as", P_IGNORED }, { "tab-size", P_TAB_SIZE }, { "table-layout", P_TABLE_LAYOUT },
    { "text-align", P_TEXT_ALIGN }, { "text-align-last", P_TEXT_ALIGN_LAST }, { "text-anchor", P_IGNORED },
    { "text-box", P_IGNORED }, { "text-box-edge", P_IGNORED }, { "text-box-trim", P_IGNORED },
    { "text-decoration", P_TEXT_DECORATION }, { "text-decoration-color", P_TEXT_DECORATION_COLOR },
    { "text-decoration-line", P_TEXT_DECORATION_LINE }, { "text-decoration-skip", P_IGNORED },
    { "text-decoration-skip-ink", P_IGNORED }, { "text-decoration-style", P_TEXT_DECORATION_STYLE },
    { "text-decoration-thickness", P_DECO_THICKNESS }, { "text-indent", P_TEXT_INDENT },
    { "text-justify", P_IGNORED }, { "text-overflow", P_TEXT_OVERFLOW }, { "text-rendering", P_IGNORED },
    { "text-shadow", P_TEXT_SHADOW }, { "text-size-adjust", P_IGNORED }, { "text-transform", P_TEXT_TRANSFORM },
    { "text-underline-offset", P_UNDERLINE_OFFSET }, { "text-underline-position", P_IGNORED },
    { "text-wrap", P_TEXT_WRAP }, { "text-wrap-mode", P_TEXT_WRAP }, { "text-wrap-style", P_IGNORED },
    { "top", P_TOP }, { "touch-action", P_IGNORED }, { "transform", P_TRANSFORM },
    { "transform-box", P_IGNORED }, { "transform-origin", P_TRANSFORM_ORIGIN },
    { "transform-style", P_IGNORED }, { "translate", P_TRANSLATE }, { "unicode-bidi", P_IGNORED },
    { "user-select", P_USER_SELECT }, { "vector-effect", P_IGNORED }, { "vertical-align", P_VERTICAL_ALIGN },
    { "view-transition-class", P_IGNORED }, { "view-transition-name", P_IGNORED },
    { "visibility", P_VISIBILITY }, { "white-space", P_WHITE_SPACE }, { "white-space-collapse", P_IGNORED },
    { "widows", P_IGNORED }, { "width", P_WIDTH }, { "will-change", P_IGNORED }, { "word-break", P_WORD_BREAK },
    { "word-spacing", P_WORD_SPACING }, { "word-wrap", P_OVERFLOW_WRAP }, { "z-index", P_Z_INDEX },
    { "zoom", P_IGNORED },
};

/* ---- selectors and rules ---- */

enum {
    PS_FIRST_CHILD = 1 << 0, PS_LAST_CHILD = 1 << 1, PS_ONLY_CHILD = 1 << 2, PS_LINK = 1 << 3,
    PS_VISITED = 1 << 4, PS_ROOT = 1 << 5, PS_EMPTY = 1 << 6, PS_NEVER = 1 << 7,
    PS_CHECKED = 1 << 8, PS_DISABLED = 1 << 9, PS_ENABLED = 1 << 10, PS_FIRST_OF_TYPE = 1 << 11,
    PS_LAST_OF_TYPE = 1 << 12, PS_ONLY_OF_TYPE = 1 << 13, PS_REQUIRED = 1 << 14, PS_OPTIONAL = 1 << 15,
    PS_ANY_LINK = 1 << 16, PS_PLACEHOLDER = 1 << 17, PS_HOVER = 1 << 18, PS_FOCUS = 1 << 19,
    PS_FOCUS_WITHIN = 1 << 20, PS_TARGET = 1 << 21, PS_READ_ONLY = 1 << 22, PS_READ_WRITE = 1 << 23,
    PS_DEFAULT = 1 << 24, PS_INDETERMINATE = 1 << 25, PS_VALID = 1 << 26, PS_INVALID = 1 << 27,
    PS_IN_RANGE = 1 << 28, PS_OUT_RANGE = 1 << 29,
};
#define PS_ACTIVE       (1u << 30)
#define PS_USER_INVALID (1u << 31)
/* :modal, :popover-open and :open: states scripts (and invoker buttons) set, outside the attributes */
enum { ST_MODAL = 1, ST_POPOVER_OPEN = 2, ST_OPEN = 4 };

/* Page zoom in percent (see web.h): the browser sets it before it parses or cascades. */
int css_zoom = 100;

/* The fragment of the page's address, for :target (set by the browser before it cascades). */
const char *css_target_id;

struct attr_sel {
    const char *name, *value;
    char op;                    /* 0 exists, '=', '~', '|', '^', '$', '*' */
    uint8_t nocase;
};

struct selector;

struct compound {
    uint8_t host;
    struct selector *host_args, *slot_args;
    int nhost, nslot;
    int tag;                    /* -1: any */
    const char *tagname;        /* for elements we have no tag id for */
    const char *id;
    const char **classes;
    int nclasses;
    uint32_t id_h, *class_h;    /* hash_str() of the id and of each class */
    struct attr_sel *attrs;
    int nattrs;
    uint32_t pseudo;
    uint8_t focus_visible;
    uint8_t state;              /* ST_*: the top layer's and <details>'s states */
    const char *lang;           /* :lang(xx) */
    int nth_a, nth_b, has_nth, nth_of_type, nth_last;
    struct selector *nth_list;   /* siblings matching the optional nth-child "of" filter */
    int nnth;
    struct selector *has_list;  /* :has(a, > b): has a descendant (or child) matching one */
    int nhas;
    uint32_t has_child;         /* bit i: has_list[i] must match a child */
    struct selector *not_list;  /* :not(a, b): matches none of them */
    int nnot;
    struct selector *is_list;   /* :is(a, b) / :where(): matches one of them */
    int nis;
    char comb;                  /* how it relates to the compound before it */
    uint8_t pe;                 /* PE_* */
};

struct selector {
    struct compound *parts;     /* left to right; the last is the subject */
    int nparts;
    uint32_t spec;
    uint8_t pe;                 /* the rule styles the element's ::before (1) or ::after (2) */
    uint8_t nanc;               /* index_sheet(): keys some ancestor must have (bloom_key()) */
    uint32_t anc[4];
};

static uint32_t hash_str(const char *s);

struct decl {
    uint8_t prop;
    uint8_t important;
    const char *name;           /* custom properties */
    const char *value;
};

/* Layers are registered in first-appearance order, including empty statements.
 * A parent's own declarations sort after its child layers. */
struct layer {
    const char *name;
    struct layer *parent, *next, *canonical;
    int order;
};

/* An @container block's query: the container's name (NULL: any) and the condition; outer is
 * the @container it sits in, if any (all of them must hold). */
struct cquery {
    const char *name, *cond;
    const struct cquery *outer;
};

struct rule {
    const struct cquery *cq;
    struct layer *layer;
    uint8_t starting;           /* inside @starting-style: only for an element's starting style */
    struct selector *sels;
    int nsels;
    struct decl *decls;
    int ndecls;
};

struct ref {                    /* a selector, filed in the index */
    int rule, sel;
    struct ref *next;
};

/* @starting-style rules in any sheet so far; while cur_starting the cascade matches them (and counts
 * how many did in starting_hits), otherwise it passes them by. */
static int css_starting_rules, cur_starting, starting_hits;

struct sheet {
    int active_starting;                /* reading inside @starting-style */
    struct layer *layers, *last_layer, *active_layer;
    const struct cquery *active_cq;     /* the @container being read */
    struct custom_prop *registered;     /* @property: names and their initial values */
    unsigned anonymous_layer;
    struct rule *rules;
    int nrules, cap;
    struct arena *a;
    /* the index: selectors by what their subject must have */
    struct ref *by_id[INDEX_BUCKETS], *by_class[INDEX_BUCKETS], *by_tag[T_COUNT], *other;
    struct keyframes *keyframes;        /* @keyframes, the last one first */
};

/* ---- the user agent style sheet ---- */

static const char ua_css[] =
    "html, address, blockquote, body, center, dialog, div, figure, figcaption, footer, form,"
    " header, hr, legend, listing, main, p, plaintext, pre, xmp, article, aside, h1, h2, h3,"
    " h4, h5, h6, hgroup, nav, section, dl, dt, dd, ol, ul, menu, dir, fieldset, details,"
    " summary, frameset, frame, noframes, optgroup { display: block }"
    "head, script, style, link, meta, title, base, template, param, area, map, source, track,"
    " datalist, embed, audio:not([controls]), col, colgroup, option,"
    " input[type=hidden], [hidden], noembed, noscript, dialog:not([open]), datalist { display: none }"
    /* <video> is a box that offers the file, <audio controls> a player (layout.c); an <object> shows
     * what it holds for browsers that cannot show it */
    "li { display: list-item }"
    "table { display: table; border-spacing: 2px; text-indent: 0 }"
    "caption { display: table-caption; text-align: center }"
    "tr { display: table-row }"
    "thead, tbody, tfoot { display: table-row-group }"
    "td, th { display: table-cell; padding: 1px; vertical-align: middle }"
    "th { font-weight: bold; text-align: center }"
    "html { font-family: serif; font-size: 16px; color: black }"
    "body { margin: 8px }"
    "p, dl, figure { margin: 1em 0 }"
    "blockquote, figure { margin: 1em 40px }"
    "h1 { font-size: 2em; margin: .67em 0; font-weight: bold }"
    "h2 { font-size: 1.5em; margin: .83em 0; font-weight: bold }"
    "h3 { font-size: 1.17em; margin: 1em 0; font-weight: bold }"
    "h4 { margin: 1.33em 0; font-weight: bold }"
    "h5 { font-size: .83em; margin: 1.67em 0; font-weight: bold }"
    "h6 { font-size: .67em; margin: 2.33em 0; font-weight: bold }"
    "ul, ol, menu, dir { margin: 1em 0; padding-left: 40px }"
    "ul, menu, dir { list-style-type: disc }"
    "ol { list-style-type: decimal }"
    "ul ul, ol ul, menu ul, ul menu { list-style-type: circle }"
    "ul ul ul, ol ul ul, ul ol ul, ol ol ul { list-style-type: square }"
    "ul ul, ul ol, ol ul, ol ol, ul menu, menu ul { margin-top: 0; margin-bottom: 0 }"
    "dd { margin-left: 40px }"
    "pre, xmp, listing, plaintext { white-space: pre; margin: 1em 0 }"
    "pre, xmp, listing, plaintext, code, kbd, samp, tt { font-family: monospace; font-size: .8125em }"
    "pre code, pre kbd, pre samp, pre tt { font-size: 1em }"
    "b, strong, dt { font-weight: bold }"
    "i, em, cite, var, dfn, address { font-style: italic }"
    "u, ins { text-decoration: underline }"
    "s, strike, del { text-decoration: line-through }"
    "small { font-size: smaller } big { font-size: larger }"
    "sub { vertical-align: sub; font-size: smaller }"
    "sup { vertical-align: super; font-size: smaller }"
    "a:link { color: #0000ee; text-decoration: underline }"
    "a:visited { color: #551a8b; text-decoration: underline }"
    "center { text-align: -webkit-center }"
    "hr { border: 1px inset #808080; margin: .5em auto; height: 0 }"
    "mark { background-color: yellow; color: black }"
    "nobr { white-space: nowrap }"
    "input, select, textarea, button { display: inline-block; font-family: sans-serif;"
    " font-size: 13px; font-weight: normal; font-style: normal; color: black;"
    " text-indent: 0; text-transform: none; line-height: normal }"
    "button { padding: 1px 6px; border: 2px outset #c0c0c0; background-color: #c0c0c0;"
    " text-align: center; white-space: nowrap }"
    "fieldset { margin: 0 2px; padding: .35em .75em .625em; border: 2px groove #c0c0c0 }"
    "legend { padding: 0 2px }"
    "details > :not(summary) { display: none }"
    "details[open] > :not(summary) { display: block }"
    "q::before { content: open-quote } q::after { content: close-quote }"
    "img { vertical-align: baseline }"
    "iframe { border: 2px inset #808080 }"
    /* the top layer: modal dialogs and open popovers sit over the page, a backdrop under each */
    "dialog { position: absolute; left: 0; right: 0; width: fit-content; height: fit-content; margin: auto;"
    " border: 2px solid; padding: 1em; background-color: white; color: black }"
    "dialog:modal { position: fixed; top: 0; bottom: 0; overflow: auto;"
    " max-width: calc(100% - 6px - 2em); max-height: calc(100% - 6px - 2em) }"
    "::backdrop { position: fixed; top: 0; right: 0; bottom: 0; left: 0 }"
    "dialog::backdrop { background-color: rgba(0, 0, 0, .1) }"
    "[popover] { position: fixed; top: 0; right: 0; bottom: 0; left: 0; width: fit-content; height: fit-content;"
    " margin: auto; border: solid; padding: .25em; overflow: auto; color: black; background-color: white }"
    "[popover]:not(:popover-open):not(dialog[open]) { display: none }"
    "dialog:popover-open { display: block }"
    "slot { display: contents }"
    "progress, meter { display: inline-block; vertical-align: middle }"
    "ruby { display: inline-flex; flex-direction: column-reverse; align-items: center;"
    " text-align: center; vertical-align: text-bottom }"
    "rt { display: block; font-size: .5em; line-height: 1.1; text-align: center }"
    "rp { display: none }";

/* In quirks mode tables do not inherit these (the HTML standard's rendering section). */
static const char ua_quirks_css[] =
    "table { font-weight: normal; font-style: normal; font-size: medium; line-height: normal;"
    " white-space: normal; text-align: left }";

static struct arena ua_arena;
static struct sheet *ua_sheet, *ua_quirks;

/* ---- low-level scanning ---- */

struct scan {
    const char *s, *end;
};

static void skip_ws(struct scan *sc)
{
    for (;;) {
        while (sc->s < sc->end && isspace((unsigned char)*sc->s))
            sc->s++;
        if (sc->end - sc->s >= 2 && sc->s[0] == '/' && sc->s[1] == '*') {
            const char *e = sc->s + 2;
            while (e + 1 < sc->end && !(e[0] == '*' && e[1] == '/'))
                e++;
            sc->s = e + 1 < sc->end ? e + 2 : sc->end;
            continue;
        }
        if (sc->end - sc->s >= 4 && !strncmp(sc->s, "<!--", 4)) {
            sc->s += 4;
            continue;
        }
        if (sc->end - sc->s >= 3 && !strncmp(sc->s, "-->", 3)) {
            sc->s += 3;
            continue;
        }
        return;
    }
}

/* Advance past a string or a bracketed group starting at *p. */
static const char *skip_group(const char *p, const char *end)
{
    char open = *p, close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}' : open;
    int depth = 0;
    if (open == '"' || open == '\'') {
        for (p++; p < end && *p != open; p++)
            if (*p == '\\' && p + 1 < end)
                p++;
        return p < end ? p + 1 : end;
    }
    for (; p < end; p++) {
        if (*p == '"' || *p == '\'') {
            p = skip_group(p, end) - 1;
        } else if (*p == '/' && p + 1 < end && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e && e < end ? e + 1 : end - 1;
        } else if (*p == open) {
            depth++;
        } else if (*p == close && --depth == 0) {
            return p + 1;
        }
    }
    return end;
}

/* Find the first `c` at nesting depth 0, or end. */
static const char *find_top(const char *p, const char *end, char c)
{
    while (p < end && *p != c) {
        if (*p == '"' || *p == '\'' || *p == '(' || *p == '[' || (*p == '{' && c != '{'))
            p = skip_group(p, end);
        else if (*p == '/' && p + 1 < end && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            p = e && e < end ? e + 2 : end;
        } else
            p++;
    }
    return p;
}

static char *trimmed(struct arena *a, const char *s, const char *e)
{
    while (s < e && isspace((unsigned char)*s))
        s++;
    while (e > s && isspace((unsigned char)e[-1]))
        e--;
    /* Drop comments inside values. */
    char *out = arena_alloc(a, (size_t)(e - s) + 1);
    if (!out)
        return NULL;
    int o = 0;
    for (const char *p = s; p < e; p++) {
        if (*p == '/' && p + 1 < e && p[1] == '*') {
            const char *c = strstr(p + 2, "*/");
            p = c && c < e ? c + 1 : e;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
    return out;
}

static int is_ident_char(char c)
{
    return isalnum((unsigned char)c) || c == '-' || c == '_' || (unsigned char)c >= 0x80 || c == '\\';
}

/* An identifier with escapes resolved ("\:" -> ":"), in the arena. */
static const char *read_ident(struct arena *a, const char **p, const char *end, int lower)
{
    char buf[256];
    int n = 0;
    const char *s = *p;
    while (s < end && is_ident_char(*s)) {
        if (*s == '\\' && s + 1 < end) {
            s++;
            if (isxdigit((unsigned char)*s)) {
                int cp = 0, k = 0;
                while (s < end && k < 6 && isxdigit((unsigned char)*s)) {
                    cp = cp * 16 + (isdigit((unsigned char)*s) ? *s - '0' : tolower((unsigned char)*s) - 'a' + 10);
                    s++, k++;
                }
                if (s < end && *s == ' ')
                    s++;
                if (n < (int)sizeof(buf) - 4)
                    n += utf8_put(buf + n, cp);
                continue;
            }
        }
        if (n < (int)sizeof(buf) - 1)
            buf[n++] = lower ? (char)tolower((unsigned char)*s) : *s;
        s++;
    }
    *p = s;
    return n ? arena_strndup(a, buf, (size_t)n) : NULL;
}

/* ---- selector parsing ---- */

static int parse_compound_list(struct arena *a, const char *s, const char *end,
                               struct selector **out, int *nout);

static uint32_t list_specificity(const struct selector *list, int count)
{
    uint32_t spec = 0;
    for (int i = 0; i < count; i++)
        if (list[i].spec > spec)
            spec = list[i].spec;
    return spec;
}

/* Parse one compound selector; returns its end, or NULL if it is invalid. */
static const char *parse_compound(struct arena *a, const char *s, const char *end,
                                  struct compound *c, uint32_t *spec)
{
    c->tag = -1;
    const char *start = s;
    if (s < end && *s == '*') {
        s++;
    } else if (s < end && is_ident_char(*s)) {
        const char *name = read_ident(a, &s, end, 1);
        if (!name)
            return NULL;
        c->tag = tag_lookup(name, (int)strlen(name));
        if (c->tag == T_UNKNOWN)
            c->tagname = name;
        *spec += 1;
    }
    if (s < end && *s == '|')
        return NULL;                    /* namespaces */
    const char *classes[16];
    struct attr_sel attrs[8];
    for (;;) {
        if (s >= end)
            break;
        if (*s == '#') {
            s++;
            c->id = read_ident(a, &s, end, 0);
            if (!c->id)
                return NULL;
            *spec += 1 << 20;
        } else if (*s == '.') {
            s++;
            const char *cls = read_ident(a, &s, end, 0);
            if (!cls)
                return NULL;
            if (c->nclasses < 16)
                classes[c->nclasses++] = cls;
            *spec += 1 << 10;
        } else if (*s == '[') {
            const char *close = skip_group(s, end);
            const char *p = s + 1, *e = close - 1;
            struct attr_sel at = { 0 };
            while (p < e && isspace((unsigned char)*p))
                p++;
            at.name = read_ident(a, &p, e, 1);
            if (!at.name)
                return NULL;
            while (p < e && isspace((unsigned char)*p))
                p++;
            if (p < e && *p != '=') {
                at.op = *p++;
                if (!strchr("~|^$*", at.op) || p >= e || *p != '=')
                    return NULL;
            } else if (p < e) {
                at.op = '=';
            }
            if (at.op) {
                p++;
                while (p < e && isspace((unsigned char)*p))
                    p++;
                if (p < e && (*p == '"' || *p == '\'')) {
                    const char *q = skip_group(p, e);
                    at.value = arena_strndup(a, p + 1, (size_t)(q - p - 2 > 0 ? q - p - 2 : 0));
                    p = q;
                } else {
                    at.value = read_ident(a, &p, e, 0);
                }
                if (!at.value)
                    at.value = "";
                while (p < e && isspace((unsigned char)*p))
                    p++;
                if (p < e && (*p == 'i' || *p == 'I'))
                    at.nocase = 1;
            }
            if (c->nattrs < 8)
                attrs[c->nattrs++] = at;
            *spec += 1 << 10;
            s = close;
        } else if (*s == ':') {
            s++;
            int element = s < end && *s == ':';
            if (element)
                s++;
            const char *name = read_ident(a, &s, end, 1);
            if (!name)
                return NULL;
            const char *args = NULL, *args_end = NULL;
            if (s < end && *s == '(') {
                const char *close = skip_group(s, end);
                args = s + 1;
                args_end = close > args && close[-1] == ')' ? close - 1 : close;    /* unclosed: to the end */
                s = close;
            }
            if (!strcmp(name, "before") || !strcmp(name, "after")) {
                c->pe = name[0] == 'b' ? PE_BEFORE : PE_AFTER;
                *spec += 1;
                continue;
            }
            if (element && (!strcmp(name, "placeholder") || !strcmp(name, "-webkit-input-placeholder") ||
                            !strcmp(name, "-moz-placeholder") || !strcmp(name, "-ms-input-placeholder"))) {
                c->pe = PE_PLACEHOLDER;
                *spec += 1;
                continue;
            }
            if (element && (!strcmp(name, "selection") || !strcmp(name, "-moz-selection"))) {
                c->pe = PE_SELECTION;
                *spec += 1;
                continue;
            }
            if (element && !strcmp(name, "marker")) {
                c->pe = PE_MARKER;
                *spec += 1;
                continue;
            }
            if (element && !strcmp(name, "backdrop")) {
                c->pe = PE_BACKDROP;
                *spec += 1;
                continue;
            }
            if (element && !strcmp(name, "slotted") && args) {
                if (parse_compound_list(a, args, args_end, &c->slot_args, &c->nslot) < 0 || c->nslot != 1 ||
                    c->slot_args[0].nparts != 1 || c->slot_args[0].pe) return NULL;
                *spec += 1 + list_specificity(c->slot_args, c->nslot);
                continue;
            }
            if (!strcmp(name, "first-letter") || !strcmp(name, "first-line")) {
                c->pe = name[7] == 'e' ? PE_FIRST_LETTER : PE_FIRST_LINE;      /* (one colon or two) */
                *spec += 1;
                continue;
            }
            if (element)
                return NULL;            /* other pseudo-elements: not generated */
            if (!strcmp(name, "host")) {
                c->host = 1;
                if (args) {
                    if (parse_compound_list(a, args, args_end, &c->host_args, &c->nhost) < 0 ||
                        c->nhost != 1 || c->host_args[0].nparts != 1 || c->host_args[0].pe) return NULL;
                    *spec += list_specificity(c->host_args, c->nhost);
                }
            } else if (!strcmp(name, "modal"))
                c->state |= ST_MODAL;
            else if (!strcmp(name, "popover-open"))
                c->state |= ST_POPOVER_OPEN;
            else if (!strcmp(name, "open"))
                c->state |= ST_OPEN;
            else if (!strcmp(name, "first-child"))
                c->pseudo |= PS_FIRST_CHILD;
            else if (!strcmp(name, "last-child"))
                c->pseudo |= PS_LAST_CHILD;
            else if (!strcmp(name, "only-child"))
                c->pseudo |= PS_ONLY_CHILD;
            else if (!strcmp(name, "first-of-type"))
                c->pseudo |= PS_FIRST_OF_TYPE;
            else if (!strcmp(name, "last-of-type"))
                c->pseudo |= PS_LAST_OF_TYPE;
            else if (!strcmp(name, "hover")) {
                c->pseudo |= PS_HOVER;
                css_dynamic_rules++;
            } else if (!strcmp(name, "active")) {
                c->pseudo |= PS_ACTIVE;
                css_dynamic_rules++;
            } else if (!strcmp(name, "scope")) {
                c->pseudo |= PS_ROOT;           /* in a document, the scope is the root element */
            } else if (!strcmp(name, "dir") && args) {
                const char *d = args;
                while (d < args_end && isspace((unsigned char)*d))
                    d++;
                if (strncasecmp(d, "ltr", 3))
                    c->pseudo |= PS_NEVER;      /* nothing here is right to left */
            } else if (!strcmp(name, "user-invalid")) {
                c->pseudo |= PS_USER_INVALID;
                css_dynamic_rules++;
            } else if (!strcmp(name, "focus-visible")) {
                c->focus_visible = 1;
                c->pseudo |= PS_FOCUS;
                css_dynamic_rules = 1;
            } else if (!strcmp(name, "focus")) {
                c->pseudo |= PS_FOCUS;
                css_dynamic_rules++;
            } else if (!strcmp(name, "focus-within")) {
                c->pseudo |= PS_FOCUS_WITHIN;
                css_dynamic_rules++;
            } else if (!strcmp(name, "only-of-type"))
                c->pseudo |= PS_ONLY_OF_TYPE;
            else if (!strcmp(name, "required"))
                c->pseudo |= PS_REQUIRED;
            else if (!strcmp(name, "optional"))
                c->pseudo |= PS_OPTIONAL;
            else if (!strcmp(name, "placeholder-shown"))
                c->pseudo |= PS_PLACEHOLDER;
            else if (!strcmp(name, "any-link"))
                c->pseudo |= PS_ANY_LINK;
            else if (!strcmp(name, "link"))
                c->pseudo |= PS_LINK;
            else if (!strcmp(name, "visited"))
                c->pseudo |= PS_VISITED;
            else if (!strcmp(name, "root"))
                c->pseudo |= PS_ROOT;
            else if (!strcmp(name, "target")) {
                c->pseudo |= PS_TARGET;
                css_dynamic_rules++;
            } else if (!strcmp(name, "read-only"))
                c->pseudo |= PS_READ_ONLY;
            else if (!strcmp(name, "read-write"))
                c->pseudo |= PS_READ_WRITE;
            else if (!strcmp(name, "default"))
                c->pseudo |= PS_DEFAULT;
            else if (!strcmp(name, "indeterminate"))
                c->pseudo |= PS_INDETERMINATE;
            else if (!strcmp(name, "valid")) {
                c->pseudo |= PS_VALID;
                css_dynamic_rules++;
            } else if (!strcmp(name, "invalid")) {
                c->pseudo |= PS_INVALID;
                css_dynamic_rules++;
            } else if (!strcmp(name, "in-range"))
                c->pseudo |= PS_IN_RANGE;
            else if (!strcmp(name, "out-of-range"))
                c->pseudo |= PS_OUT_RANGE;
            else if (!strcmp(name, "lang") && args) {
                const char *l = args;
                while (l < args_end && (isspace((unsigned char)*l) || *l == '"' || *l == '\''))
                    l++;
                const char *le = args_end;
                while (le > l && (isspace((unsigned char)le[-1]) || le[-1] == '"' || le[-1] == '\''))
                    le--;
                c->lang = arena_strndup(a, l, (size_t)(le - l));
                if (!c->lang)
                    return NULL;
            }
            else if (!strcmp(name, "empty"))
                c->pseudo |= PS_EMPTY;
            else if (!strcmp(name, "checked"))
                c->pseudo |= PS_CHECKED;
            else if (!strcmp(name, "disabled"))
                c->pseudo |= PS_DISABLED;
            else if (!strcmp(name, "enabled"))
                c->pseudo |= PS_ENABLED;
            else if ((!strcmp(name, "nth-child") || !strcmp(name, "nth-of-type") ||
                      !strcmp(name, "nth-last-child") || !strcmp(name, "nth-last-of-type")) && args) {
                const char *filter;
                if (web_parse_nth(args, args_end, &c->nth_a, &c->nth_b, &filter) < 0)
                    return NULL;
                c->has_nth = 1;
                c->nth_last = !strncmp(name, "nth-last", 8);
                c->nth_of_type = strstr(name, "of-type") != NULL;
                if (filter) {
                    if (c->nth_of_type ||
                        parse_compound_list(a, filter, args_end, &c->nth_list, &c->nnth) < 0)
                        return NULL;
                    uint32_t max_spec = 0;
                    for (int i = 0; i < c->nnth; i++) {
                        if (c->nth_list[i].pe)
                            return NULL;
                        if (c->nth_list[i].spec > max_spec)
                            max_spec = c->nth_list[i].spec;
                    }
                    *spec += max_spec;
                }
            } else if (!strcmp(name, "has") && args) {
                /* a leading '>' means children only; other relative selectors never match */
                char buf[256];
                snprintf(buf, sizeof(buf), "%.*s", (int)(args_end - args), args);
                const char *p = buf;
                int child = 0;
                while (isspace((unsigned char)*p))
                    p++;
                if (*p == '>') {
                    child = 1;
                    p++;
                } else if (*p == '+' || *p == '~') {
                    c->pseudo |= PS_NEVER;
                    *spec += 1 << 10;
                    continue;
                }
                const char *copy = arena_strdup(a, p);
                if (!copy || parse_compound_list(a, copy, copy + strlen(copy), &c->has_list, &c->nhas) < 0)
                    return NULL;
                c->has_child = child ? 0xFFFFFFFFu : 0;
                *spec += list_specificity(c->has_list, c->nhas);
                continue;
            } else if (!strcmp(name, "not") && args) {
                struct selector *list;
                int count;
                if (parse_compound_list(a, args, args_end, &list, &count) < 0)
                    return NULL;
                *spec += list_specificity(list, count);
                if (c->nnot) {                  /* :not(a):not(b) is :not(a, b) */
                    struct selector *both = arena_alloc(a, sizeof(*both) * (size_t)(c->nnot + count));
                    if (!both)
                        return NULL;
                    memcpy(both, c->not_list, sizeof(*both) * (size_t)c->nnot);
                    memcpy(both + c->nnot, list, sizeof(*both) * (size_t)count);
                    list = both;
                    count += c->nnot;
                }
                c->not_list = list;
                c->nnot = count;
                continue;
            } else if ((!strcmp(name, "is") || !strcmp(name, "matches") || !strcmp(name, "where") ||
                        !strcmp(name, "-webkit-any")) && args) {
                if (parse_compound_list(a, args, args_end, &c->is_list, &c->nis) < 0)
                    return NULL;
                if (name[0] != 'w')     /* :where() adds nothing */
                    *spec += list_specificity(c->is_list, c->nis);
                continue;
            } else {
                c->pseudo |= PS_NEVER;  /* :hover, :focus and anything we do not know */
            }
            *spec += 1 << 10;
        } else {
            break;
        }
    }
    if (s == start)
        return NULL;
    if (c->nclasses) {
        c->classes = arena_alloc(a, sizeof(char *) * (size_t)c->nclasses);
        c->class_h = arena_alloc(a, sizeof(uint32_t) * (size_t)c->nclasses);
        if (!c->classes || !c->class_h)
            return NULL;
        memcpy(c->classes, classes, sizeof(char *) * (size_t)c->nclasses);
        for (int i = 0; i < c->nclasses; i++)
            c->class_h[i] = hash_str(classes[i]);
    }
    if (c->id)
        c->id_h = hash_str(c->id);
    if (c->nattrs) {
        c->attrs = arena_alloc(a, sizeof(struct attr_sel) * (size_t)c->nattrs);
        if (!c->attrs)
            return NULL;
        memcpy(c->attrs, attrs, sizeof(struct attr_sel) * (size_t)c->nattrs);
    }
    return s;
}

static int parse_selector(struct arena *a, const char *s, const char *end, struct selector *sel);

static int compound_list(struct arena *a, const char *s, const char *end, struct selector **out, int *nout);

/* "a, b > c" inside :not() or :is(): a list of full selectors. Each level
 * takes a few KB of stack (perhaps a script's coroutine's), so few levels. */
static int parse_compound_list(struct arena *a, const char *s, const char *end,
                               struct selector **out, int *nout)
{
    static int nesting;
    if (nesting >= 8)
        return -1;
    nesting++;
    int r = compound_list(a, s, end, out, nout);
    nesting--;
    return r;
}

static int compound_list(struct arena *a, const char *s, const char *end, struct selector **out, int *nout)
{
    struct selector list[16];
    int n = 0;
    while (s < end) {
        const char *e = s;
        int depth = 0;
        for (; e < end && (depth || *e != ','); e++) {
            if (*e == '(' || *e == '[')
                depth++;
            else if ((*e == ')' || *e == ']') && depth)
                depth--;
        }
        const char *t = s;
        while (t < e && isspace((unsigned char)*t))
            t++;
        if (t < e) {
            memset(&list[n < 15 ? n : 15], 0, sizeof(list[0]));
            if (parse_selector(a, t, e, &list[n < 15 ? n : 15]) < 0)
                return -1;
            if (n < 15)
                n++;
        }
        s = e < end ? e + 1 : e;
    }
    if (!n)
        return -1;
    *out = arena_alloc(a, sizeof(struct selector) * (size_t)n);
    if (!*out)
        return -1;
    memcpy(*out, list, sizeof(struct selector) * (size_t)n);
    *nout = n;
    return 0;
}

static int parse_selector(struct arena *a, const char *s, const char *end, struct selector *sel)
{
    struct compound parts[MAX_PARTS];
    int n = 0;
    uint32_t spec = 0;
    char comb = 0;
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    while (s < end) {
        while (s < end && isspace((unsigned char)*s)) {
            s++;
            if (n)
                comb = comb ? comb : ' ';
        }
        if (s >= end)
            break;
        if (*s == '>' || *s == '+' || *s == '~') {
            if (!n)
                return -1;
            comb = *s++;
            continue;
        }
        if (n == MAX_PARTS)
            return -1;
        memset(&parts[n], 0, sizeof(parts[n]));
        const char *e = parse_compound(a, s, end, &parts[n], &spec);
        if (!e)
            return -1;
        parts[n].comb = n ? (comb ? comb : ' ') : 0;
        comb = 0;
        n++;
        s = e;
    }
    if (!n || comb)
        return -1;
    for (int i = 0; i < n - 1; i++)
        if (parts[i].pe)
            return -1;                  /* ::before must end the selector */
    sel->pe = parts[n - 1].pe;
    sel->parts = arena_alloc(a, sizeof(struct compound) * (size_t)n);
    if (!sel->parts)
        return -1;
    memcpy(sel->parts, parts, sizeof(struct compound) * (size_t)n);
    sel->nparts = n;
    sel->spec = spec;
    return 0;
}

/* ---- declarations ---- */

static int prop_lookup(const char *name)
{
    const char *motion = !strncmp(name, "-webkit-", 8) || !strncmp(name, "-moz-", 5) ? strchr(name + 1, '-') + 1 : name;
    if (!strncmp(motion, "transition", 10) || !strncmp(motion, "animation", 9)) {
        for (unsigned i = 0; i < sizeof(motion_names) / sizeof(motion_names[0]); i++)
            if (!strcmp(motion_names[i].name, motion))
                return motion_names[i].prop;
        return P_UNKNOWN;
    }
    int lo = 0, hi = (int)(sizeof(prop_names) / sizeof(prop_names[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(prop_names[mid].name, name);
        if (!c)
            return prop_names[mid].prop;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return P_UNKNOWN;
}

/* Parse "a: b; c: d !important" into decls; returns how many. */
static int parse_decls(struct arena *a, const char *s, const char *end, struct decl **out)
{
    int cap = 8, n = 0;
    struct decl *d = arena_alloc(a, sizeof(*d) * (size_t)cap);
    if (!d)
        return 0;
    while (s < end) {
        const char *semi = find_top(s, end, ';');
        const char *colon = find_top(s, semi, ':');
        if (colon < semi) {
            struct scan sc = { s, colon };
            skip_ws(&sc);
            const char *name_s = sc.s, *name_e = colon;
            while (name_e > name_s && isspace((unsigned char)name_e[-1]))
                name_e--;
            char name[64];
            int nl = (int)(name_e - name_s);
            int custom = nl > 2 && name_s[0] == '-' && name_s[1] == '-';
            if (nl > 0 && nl < (int)sizeof(name) && (custom || !memchr(name_s, '{', (size_t)nl))) {
                for (int i = 0; i < nl; i++)
                    name[i] = custom ? name_s[i] : (char)tolower((unsigned char)name_s[i]);
                name[nl] = 0;
                int prop = custom ? P_CUSTOM : prop_lookup(name);
                if (prop != P_UNKNOWN) {
                    const char *ve = semi;
                    int important = 0;
                    const char *bang = find_top(colon + 1, ve, '!');
                    if (bang < ve) {
                        const char *p = bang + 1;
                        while (p < ve && isspace((unsigned char)*p))
                            p++;
                        if (!strncasecmp(p, "important", 9)) {
                            important = 1;
                            ve = bang;
                        }
                    }
                    char *value = trimmed(a, colon + 1, ve);
                    if (value && (value[0] || custom)) {
                        if (n == cap) {
                            struct decl *nd = arena_alloc(a, sizeof(*nd) * (size_t)cap * 2);
                            if (!nd)
                                break;
                            memcpy(nd, d, sizeof(*d) * (size_t)n);
                            d = nd;
                            cap *= 2;
                        }
                        d[n].prop = (uint8_t)prop;
                        d[n].important = (uint8_t)important;
                        d[n].name = custom ? arena_strdup(a, name) : NULL;
                        d[n].value = value;
                        n++;
                    }
                }
            }
        }
        s = semi < end ? semi + 1 : end;
    }
    *out = d;
    return n;
}

/* ---- transitions and animations: timing, times and @keyframes ---- */

/* The next token of a value list, spaces apart (a function's parentheses keep theirs). */
static int motion_token(const char **p, const char *end, const char **ts)
{
    while (*p < end && isspace((unsigned char)**p))
        (*p)++;
    *ts = *p;
    int depth = 0;
    while (*p < end && (depth || !isspace((unsigned char)**p))) {
        if (**p == '(')
            depth++;
        else if (**p == ')' && depth)
            depth--;
        (*p)++;
    }
    return (int)(*p - *ts);
}

/* "0.3s", "300ms", "-1s": milliseconds into *ms. */
static int parse_time(const char *s, int n, int *ms)
{
    char buf[32];
    if (n <= 0 || n >= (int)sizeof(buf))
        return 0;
    memcpy(buf, s, (size_t)n);
    buf[n] = 0;
    char *e;
    double v = strtod(buf, &e);
    if (e == buf)
        return 0;
    if (!strcasecmp(e, "ms"))
        *ms = (int)v;
    else if (!strcasecmp(e, "s"))
        *ms = (int)(v * 1000 + (v < 0 ? -0.5 : 0.5));
    else
        return 0;
    return 1;
}

static int timing_keyword(const char *s, int n, struct css_timing *t)
{
    static const struct { const char *kw; float x1, y1, x2, y2; } bez[] = {
        { "ease", .25f, .1f, .25f, 1 }, { "linear", 0, 0, 1, 1 }, { "ease-in", .42f, 0, 1, 1 },
        { "ease-out", 0, 0, .58f, 1 }, { "ease-in-out", .42f, 0, .58f, 1 },
    };
    memset(t, 0, sizeof(*t));
    for (unsigned i = 0; i < sizeof(bez) / sizeof(bez[0]); i++)
        if ((int)strlen(bez[i].kw) == n && !strncasecmp(s, bez[i].kw, (size_t)n)) {
            t->x1 = bez[i].x1, t->y1 = bez[i].y1, t->x2 = bez[i].x2, t->y2 = bez[i].y2;
            return 1;
        }
    if ((n == 10 && !strncasecmp(s, "step-start", 10)) || (n == 8 && !strncasecmp(s, "step-end", 8))) {
        t->steps_kind = 1;
        t->steps = 1;
        t->jump = n == 10 ? JUMP_START : JUMP_END;
        return 1;
    }
    if (n > 13 && !strncasecmp(s, "cubic-bezier(", 13)) {
        float v[4];
        const char *p = s + 13;
        for (int i = 0; i < 4; i++) {
            char *e;
            v[i] = strtof(p, &e);
            if (e == p)
                return 0;
            p = e;
            while (*p == ',' || isspace((unsigned char)*p))
                p++;
        }
        t->x1 = v[0] < 0 ? 0 : v[0] > 1 ? 1 : v[0], t->y1 = v[1];
        t->x2 = v[2] < 0 ? 0 : v[2] > 1 ? 1 : v[2], t->y2 = v[3];
        return 1;
    }
    if (n > 6 && !strncasecmp(s, "steps(", 6)) {
        t->steps_kind = 1;
        t->steps = atoi(s + 6);
        if (t->steps < 1)
            t->steps = 1;
        t->jump = JUMP_END;
        const char *comma = memchr(s, ',', (size_t)n);
        if (comma) {
            while (++comma < s + n && isspace((unsigned char)*comma))
                ;
            if (!strncasecmp(comma, "jump-start", 10) || !strncasecmp(comma, "start", 5))
                t->jump = JUMP_START;
            else if (!strncasecmp(comma, "jump-none", 9))
                t->jump = JUMP_NONE;
            else if (!strncasecmp(comma, "jump-both", 9))
                t->jump = JUMP_BOTH;
        }
        return 1;
    }
    if (n > 7 && !strncasecmp(s, "linear(", 7)) {     /* linear() with stops: taken as linear */
        t->x2 = t->y2 = 1;
        return 1;
    }
    return 0;
}

/* The animatable properties a declaration of prop sets (keyframes: what they animate). */
static uint64_t prop_ap_bits(int prop)
{
    const uint64_t margins = AP_BIT(AP_MARGIN_TOP) | AP_BIT(AP_MARGIN_RIGHT) | AP_BIT(AP_MARGIN_BOTTOM) |
                             AP_BIT(AP_MARGIN_LEFT);
    const uint64_t paddings = AP_BIT(AP_PADDING_TOP) | AP_BIT(AP_PADDING_RIGHT) | AP_BIT(AP_PADDING_BOTTOM) |
                              AP_BIT(AP_PADDING_LEFT);
    const uint64_t bwidths = AP_BIT(AP_BORDER_TOP_WIDTH) | AP_BIT(AP_BORDER_RIGHT_WIDTH) |
                             AP_BIT(AP_BORDER_BOTTOM_WIDTH) | AP_BIT(AP_BORDER_LEFT_WIDTH);
    const uint64_t bcolors = AP_BIT(AP_BORDER_TOP_COLOR) | AP_BIT(AP_BORDER_RIGHT_COLOR) |
                             AP_BIT(AP_BORDER_BOTTOM_COLOR) | AP_BIT(AP_BORDER_LEFT_COLOR);
    const uint64_t radii = AP_BIT(AP_BORDER_TL_RADIUS) | AP_BIT(AP_BORDER_TR_RADIUS) |
                           AP_BIT(AP_BORDER_BR_RADIUS) | AP_BIT(AP_BORDER_BL_RADIUS);
    switch (prop) {
    case P_DISPLAY: return AP_BIT(AP_DISPLAY);
    case P_COLOR: return AP_BIT(AP_COLOR);
    case P_BACKGROUND: case P_BACKGROUND_COLOR: return AP_BIT(AP_BACKGROUND_COLOR);
    case P_OPACITY: return AP_BIT(AP_OPACITY);
    case P_WIDTH: return AP_BIT(AP_WIDTH);
    case P_HEIGHT: return AP_BIT(AP_HEIGHT);
    case P_MIN_WIDTH: return AP_BIT(AP_MIN_WIDTH);
    case P_MAX_WIDTH: return AP_BIT(AP_MAX_WIDTH);
    case P_MIN_HEIGHT: return AP_BIT(AP_MIN_HEIGHT);
    case P_MAX_HEIGHT: return AP_BIT(AP_MAX_HEIGHT);
    case P_MARGIN: case P_MARGIN_INLINE: case P_MARGIN_BLOCK: return margins;
    case P_MARGIN_TOP: return AP_BIT(AP_MARGIN_TOP);
    case P_MARGIN_RIGHT: return AP_BIT(AP_MARGIN_RIGHT);
    case P_MARGIN_BOTTOM: return AP_BIT(AP_MARGIN_BOTTOM);
    case P_MARGIN_LEFT: return AP_BIT(AP_MARGIN_LEFT);
    case P_PADDING: case P_PADDING_INLINE: case P_PADDING_BLOCK: return paddings;
    case P_PADDING_TOP: return AP_BIT(AP_PADDING_TOP);
    case P_PADDING_RIGHT: return AP_BIT(AP_PADDING_RIGHT);
    case P_PADDING_BOTTOM: return AP_BIT(AP_PADDING_BOTTOM);
    case P_PADDING_LEFT: return AP_BIT(AP_PADDING_LEFT);
    case P_INSET: case P_INSET_INLINE: case P_INSET_BLOCK:
        return AP_BIT(AP_TOP) | AP_BIT(AP_RIGHT) | AP_BIT(AP_BOTTOM) | AP_BIT(AP_LEFT);
    case P_TOP: return AP_BIT(AP_TOP);
    case P_RIGHT: return AP_BIT(AP_RIGHT);
    case P_BOTTOM: return AP_BIT(AP_BOTTOM);
    case P_LEFT: return AP_BIT(AP_LEFT);
    case P_BORDER: return bwidths | bcolors;
    case P_BORDER_WIDTH: return bwidths;
    case P_BORDER_COLOR: return bcolors;
    case P_BORDER_TOP: return AP_BIT(AP_BORDER_TOP_WIDTH) | AP_BIT(AP_BORDER_TOP_COLOR);
    case P_BORDER_RIGHT: return AP_BIT(AP_BORDER_RIGHT_WIDTH) | AP_BIT(AP_BORDER_RIGHT_COLOR);
    case P_BORDER_BOTTOM: return AP_BIT(AP_BORDER_BOTTOM_WIDTH) | AP_BIT(AP_BORDER_BOTTOM_COLOR);
    case P_BORDER_LEFT: return AP_BIT(AP_BORDER_LEFT_WIDTH) | AP_BIT(AP_BORDER_LEFT_COLOR);
    case P_BORDER_TOP_WIDTH: return AP_BIT(AP_BORDER_TOP_WIDTH);
    case P_BORDER_RIGHT_WIDTH: return AP_BIT(AP_BORDER_RIGHT_WIDTH);
    case P_BORDER_BOTTOM_WIDTH: return AP_BIT(AP_BORDER_BOTTOM_WIDTH);
    case P_BORDER_LEFT_WIDTH: return AP_BIT(AP_BORDER_LEFT_WIDTH);
    case P_BORDER_TOP_COLOR: return AP_BIT(AP_BORDER_TOP_COLOR);
    case P_BORDER_RIGHT_COLOR: return AP_BIT(AP_BORDER_RIGHT_COLOR);
    case P_BORDER_BOTTOM_COLOR: return AP_BIT(AP_BORDER_BOTTOM_COLOR);
    case P_BORDER_LEFT_COLOR: return AP_BIT(AP_BORDER_LEFT_COLOR);
    case P_OUTLINE: return AP_BIT(AP_OUTLINE_COLOR) | AP_BIT(AP_OUTLINE_WIDTH);
    case P_OUTLINE_COLOR: return AP_BIT(AP_OUTLINE_COLOR);
    case P_OUTLINE_WIDTH: return AP_BIT(AP_OUTLINE_WIDTH);
    case P_OUTLINE_OFFSET: return AP_BIT(AP_OUTLINE_OFFSET);
    case P_FONT: return AP_BIT(AP_FONT_SIZE) | AP_BIT(AP_LINE_HEIGHT);
    case P_FONT_SIZE: return AP_BIT(AP_FONT_SIZE);
    case P_LETTER_SPACING: return AP_BIT(AP_LETTER_SPACING);
    case P_WORD_SPACING: return AP_BIT(AP_WORD_SPACING);
    case P_LINE_HEIGHT: return AP_BIT(AP_LINE_HEIGHT);
    case P_BORDER_RADIUS: return radii;
    case P_BORDER_TL_RADIUS: return AP_BIT(AP_BORDER_TL_RADIUS);
    case P_BORDER_TR_RADIUS: return AP_BIT(AP_BORDER_TR_RADIUS);
    case P_BORDER_BR_RADIUS: return AP_BIT(AP_BORDER_BR_RADIUS);
    case P_BORDER_BL_RADIUS: return AP_BIT(AP_BORDER_BL_RADIUS);
    case P_TRANSFORM: return AP_BIT(AP_TRANSFORM);
    case P_TRANSFORM_ORIGIN: return AP_BIT(AP_TRANSFORM_ORIGIN);
    case P_BOX_SHADOW: return AP_BIT(AP_BOX_SHADOW);
    case P_TEXT_SHADOW: return AP_BIT(AP_TEXT_SHADOW);
    case P_FLEX: return AP_BIT(AP_FLEX_GROW) | AP_BIT(AP_FLEX_SHRINK) | AP_BIT(AP_FLEX_BASIS);
    case P_FLEX_GROW: return AP_BIT(AP_FLEX_GROW);
    case P_FLEX_SHRINK: return AP_BIT(AP_FLEX_SHRINK);
    case P_FLEX_BASIS: return AP_BIT(AP_FLEX_BASIS);
    case P_GAP: return AP_BIT(AP_ROW_GAP) | AP_BIT(AP_COLUMN_GAP);
    case P_ROW_GAP: return AP_BIT(AP_ROW_GAP);
    case P_COLUMN_GAP: return AP_BIT(AP_COLUMN_GAP);
    case P_TEXT_INDENT: return AP_BIT(AP_TEXT_INDENT);
    case P_Z_INDEX: return AP_BIT(AP_Z_INDEX);
    case P_VISIBILITY: return AP_BIT(AP_VISIBILITY);
    default: return 0;
    }
}

static int frame_cmp(const void *a, const void *b)
{
    const struct kframe *x = a, *y = b;
    return x->offset < y->offset ? -1 : x->offset > y->offset;
}

/* @keyframes name { from {...} 50% {...} to {...} } (s and end: what is between the braces). */
static void parse_keyframes(struct sheet *sh, const char *ns, const char *ne, const char *s, const char *end)
{
    struct arena *a = sh->a;
    while (ns < ne && isspace((unsigned char)*ns))
        ns++;
    while (ne > ns && isspace((unsigned char)ne[-1]))
        ne--;
    if (ne - ns >= 2 && (*ns == '"' || *ns == '\'') && ne[-1] == *ns)
        ns++, ne--;
    if (ns >= ne)
        return;
    struct keyframes *k = arena_alloc(a, sizeof(*k));
    if (!k)
        return;
    memset(k, 0, sizeof(*k));
    k->name = trimmed(a, ns, ne);
    int cap = 0;
    struct scan sc = { s, end };
    while (sc.s < sc.end) {
        skip_ws(&sc);
        const char *brace = find_top(sc.s, sc.end, '{');
        if (brace >= sc.end)
            break;
        const char *close = skip_group(brace, sc.end);
        struct decl *d;
        int nd = parse_decls(a, brace + 1, close - 1, &d);
        const struct css_timing *ease = NULL;
        uint64_t props = 0;
        for (int i = 0; i < nd; i++) {
            if (d[i].prop == P_ANIMATION_TIMING) {
                struct css_timing *t = arena_alloc(a, sizeof(*t));
                if (t && timing_keyword(d[i].value, (int)strlen(d[i].value), t))
                    ease = t;
            } else if (!d[i].important) {
                props |= prop_ap_bits(d[i].prop);
            }
        }
        for (const char *p = sc.s; p < brace;) {        /* from, to, 25%, ... */
            const char *comma = find_top(p, brace, ',');
            while (p < comma && isspace((unsigned char)*p))
                p++;
            float off = -1;
            if (!strncasecmp(p, "from", 4))
                off = 0;
            else if (!strncasecmp(p, "to", 2))
                off = 1;
            else if (isdigit((unsigned char)*p) || *p == '.')
                off = strtof(p, NULL) / 100;
            if (off >= 0 && off <= 1) {
                if (k->n == cap) {
                    int nc = cap ? cap * 2 : 4;
                    struct kframe *f = arena_alloc(a, sizeof(*f) * (size_t)nc);
                    if (!f)
                        return;
                    if (k->n)
                        memcpy(f, k->f, sizeof(*f) * (size_t)k->n);
                    k->f = f;
                    cap = nc;
                }
                k->f[k->n++] = (struct kframe){ off, ease, props, d, nd };
            }
            p = comma < brace ? comma + 1 : brace;
        }
        sc.s = close;
    }
    /* By offset, keeping the written order of frames at the same one (insertion sort: stable). */
    for (int i = 1; i < k->n; i++)
        for (int j = i; j > 0 && frame_cmp(&k->f[j - 1], &k->f[j]) > 0; j--) {
            struct kframe t = k->f[j];
            k->f[j] = k->f[j - 1];
            k->f[j - 1] = t;
        }
    k->next = sh->keyframes;
    sh->keyframes = k;
}

/* ---- media queries ---- */


static int length_px(const char *s, int base);

int css_viewport_h = 600;

/* One side of a range: "width >= 600px" compares v (the viewport's width or height). */
static int media_compare(int v, const char *op, int px, int flip)
{
    int ge = op[0] == '>' && op[1] == '=', le = op[0] == '<' && op[1] == '=';
    int gt = op[0] == '>' && !ge, lt = op[0] == '<' && !le;
    if (flip) {                         /* "600px < width": the other way round */
        int t;
        t = ge, ge = le, le = t;
        t = gt, gt = lt, lt = t;
    }
    return ge ? v >= px : le ? v <= px : gt ? v > px : lt ? v < px : v == px;
}

/* "1.5" as 150: a number times 100. */
static int fixed100(const char *s)
{
    int v = atoi(s) * 100, k = 0, frac = 0;
    const char *dot = strchr(s, '.');
    if (dot)
        for (const char *p = dot + 1; isdigit((unsigned char)*p) && k < 2; p++, k++)
            frac = frac * 10 + (*p - '0');
    return v + (k == 1 ? frac * 10 : frac);
}

/* aspect-ratio values: "16/9", "16 / 9" or "1.5", as times 100. */
static int ratio_value(const char *s)
{
    const char *slash = strchr(s, '/');
    if (!slash)
        return fixed100(s);
    int b = atoi(slash + 1);
    return b > 0 ? atoi(s) * 100 / b : 0;
}

static int size_feature(const char *f, int vw, int vh)
{
    /* "(min-width: 600px)", "(width >= 600px)", "(400px <= width <= 700px)", "(prefers-color-scheme: dark)" */
    char buf[128];
    int n = 0;
    for (; *f && *f != ')' && n < 127; f++)
        if (*f != '(')
            buf[n++] = (char)tolower((unsigned char)*f);
    buf[n] = 0;
    char *colon = strchr(buf, ':');
    if (colon) {
        *colon = 0;
        char *name = buf, *value = colon + 1;
        while (*name == ' ')
            name++;
        while (*value == ' ')
            value++;
        char *ne = name + strlen(name);
        while (ne > name && ne[-1] == ' ')
            *--ne = 0;
        if (!strcmp(name, "min-width"))
            return vw >= length_px(value, 16);
        if (!strcmp(name, "max-width"))
            return vw <= length_px(value, 16);
        if (!strcmp(name, "width"))
            return vw == length_px(value, 16);
        if (!strcmp(name, "min-height"))
            return vh >= length_px(value, 16);
        if (!strcmp(name, "max-height"))
            return vh <= length_px(value, 16);
        if (!strcmp(name, "height"))
            return vh == length_px(value, 16);
        if (!strcmp(name, "min-aspect-ratio"))
            return vw * 100 >= ratio_value(value) * vh;
        if (!strcmp(name, "max-aspect-ratio"))
            return vw * 100 <= ratio_value(value) * vh;
        if (!strcmp(name, "prefers-color-scheme"))
            return !strncmp(value, "light", 5);
        if (!strcmp(name, "orientation"))
            return !strncmp(value, vw >= vh ? "landscape" : "portrait", 8);
        if (!strcmp(name, "prefers-reduced-motion") || !strcmp(name, "prefers-reduced-transparency") ||
            !strcmp(name, "prefers-reduced-data") || !strcmp(name, "prefers-contrast"))
            return !strncmp(value, "no-preference", 13);
        if (!strcmp(name, "forced-colors") || !strcmp(name, "inverted-colors"))
            return !strncmp(value, "none", 4);
        if (!strcmp(name, "scripting"))
            return !strncmp(value, "enabled", 7);
        if (!strcmp(name, "update"))
            return !strncmp(value, "fast", 4);
        if (!strcmp(name, "display-mode"))
            return !strncmp(value, "browser", 7);
        if (!strcmp(name, "color-gamut") || !strcmp(name, "dynamic-range") || !strcmp(name, "video-dynamic-range"))
            return !strncmp(value, "srgb", 4) || !strncmp(value, "standard", 8);
        if (!strcmp(name, "min-resolution") || !strcmp(name, "max-resolution") || !strcmp(name, "resolution") ||
            strstr(name, "device-pixel-ratio")) {
            int dppx = strstr(value, "dpi") ? atoi(value) * 100 / 96 : strstr(value, "dpcm") ? atoi(value) * 254 / 96
                     : fixed100(value);
            return name[1] == 'i' || !strncmp(name, "-webkit-min", 11) ? dppx <= 100
                 : name[1] == 'a' || !strncmp(name, "-webkit-max", 11) ? dppx >= 100 : dppx == 100;
        }
        if (!strcmp(name, "color") || !strcmp(name, "min-color"))
            return atoi(value) <= 8;
        if (!strcmp(name, "monochrome"))
            return atoi(value) == 0;
        if (!strcmp(name, "hover") || !strcmp(name, "any-hover"))
            return !strncmp(value, "hover", 5);
        if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer"))
            return !strncmp(value, "fine", 4);
        if (!strcmp(name, "min-device-width"))
            return 1024 >= length_px(value, 16);
        if (!strcmp(name, "max-device-width"))
            return 1024 <= length_px(value, 16);
        return 0;
    }
    /* Range syntax: "width >= 600px", "600px < width", "400px <= width <= 700px" */
    char *op = strpbrk(buf, "<>=");
    char *feat = strstr(buf, "width") ? strstr(buf, "width") : strstr(buf, "height");
    if (op && feat) {
        int v = feat[0] == 'w' ? vw : vh;
        char *op2 = strpbrk(feat, "<>=");
        int ok = 1;
        if (op < feat) {                /* a value before the name: "400px <= width" */
            ok = media_compare(v, op, length_px(buf, 16), 1);
        }
        if (op2) {                      /* and after it: "width <= 700px" */
            char *val = op2;
            while (*val == '<' || *val == '>' || *val == '=' || *val == ' ')
                val++;
            ok = ok && media_compare(v, op2, length_px(val, 16), 0);
        }
        return ok;
    }
    if (!strncmp(buf, "color", 5) || !strcmp(buf, "hover"))
        return 1;
    return 0;
}

static int media_feature(const char *f, int vw)
{
    return size_feature(f, vw, css_viewport_h);
}

/* ---- @container ---- */

static int cq_guess_w = 1024, cq_guess_h = 768;      /* a container never laid out: the viewport */

/* A container's content box: as the last layout found it, or the guess. The cascade notes what it
 * took, so the browser can tell when the page must be styled again (layout_cq_changed). */
static void cq_size(struct node *c, int *w, int *h)
{
    *w = c->cq_known ? c->cq_w : cq_guess_w;
    *h = c->cq_known ? c->cq_h : cq_guess_h;
    c->cq_asked = 1;
    c->cq_aw = *w;
    c->cq_ah = *h;
}

/* The nearest container from n up (n included) that a query naming name (NULL: any) can use. */
static struct node *cq_container(struct node *n, const char *name)
{
    size_t nl = name ? strlen(name) : 0;
    for (; n; n = n->parent) {
        const struct style *st = n->type == N_ELEMENT ? n->style : NULL;
        if (!st || !st->cq_type)
            continue;
        if (!name)
            return n;
        for (const char *p = st->cq_name; p && *p;) {
            while (isspace((unsigned char)*p))
                p++;
            const char *e = p;
            while (*e && !isspace((unsigned char)*e))
                e++;
            if ((size_t)(e - p) == nl && !strncmp(p, name, nl))
                return n;
            p = e;
        }
    }
    return NULL;
}

/* One size feature, "min-width: 400px" or "inline-size > 30em"; h < 0: the height is not known. */
static int cq_feature(const char *s, const char *e, int w, int h)
{
    char buf[128];
    int n = 0;
    for (const char *p = s; p < e && n < 120; p++) {
        if (e - p >= 11 && !strncasecmp(p, "inline-size", 11)) {
            memcpy(buf + n, "width", 5), n += 5, p += 10;
        } else if (e - p >= 10 && !strncasecmp(p, "block-size", 10)) {
            memcpy(buf + n, "height", 6), n += 6, p += 9;
        } else {
            buf[n++] = (char)tolower((unsigned char)*p);
        }
    }
    buf[n] = 0;
    if (h < 0 && (strstr(buf, "height") || strstr(buf, "aspect-ratio") || strstr(buf, "orientation")))
        return 0;
    if (!strstr(buf, "width") && !strstr(buf, "height") && !strstr(buf, "aspect-ratio") && !strstr(buf, "orientation"))
        return 0;                               /* not a size feature */
    return size_feature(buf, w, h < 0 ? 0 : h);
}

static int word_at(const char *p, const char *e, const char *w);
static int cq_expr(const char **pp, const char *e, int w, int h);

/* not X, (X), (feature), or a function (style() and the rest, which never hold here) */
static int cq_term(const char **pp, const char *e, int w, int h)
{
    const char *p = *pp;
    while (p < e && isspace((unsigned char)*p))
        p++;
    if (word_at(p, e, "not")) {
        *pp = p + 3;
        return !cq_term(pp, e, w, h);
    }
    if (p < e && *p == '(') {
        const char *close = skip_group(p, e), *in = p + 1, *ie = close - 1;
        *pp = close;
        while (in < ie && isspace((unsigned char)*in))
            in++;
        if (in < ie && (*in == '(' || word_at(in, ie, "not")))
            return cq_expr(&in, ie, w, h);
        return cq_feature(in, ie, w, h);
    }
    while (p < e && *p != '(' && !isspace((unsigned char)*p))
        p++;
    *pp = p < e && *p == '(' ? skip_group(p, e) : p;
    return 0;
}

static int cq_expr(const char **pp, const char *e, int w, int h)
{
    int r = cq_term(pp, e, w, h);
    for (;;) {
        const char *p = *pp;
        while (p < e && isspace((unsigned char)*p))
            p++;
        if (word_at(p, e, "and")) {
            *pp = p + 3;
            r = cq_term(pp, e, w, h) && r;
        } else if (word_at(p, e, "or")) {
            *pp = p + 2;
            r = cq_term(pp, e, w, h) || r;
        } else {
            return r;
        }
    }
}

/* Does every @container round a rule hold for n (or, for a pseudo-element, n's own box)? */
static int cq_matches(const struct cquery *q, struct node *n, int pe)
{
    for (; q; q = q->outer) {
        struct node *c = cq_container(pe ? n : n->parent, q->name);
        if (!c)
            return 0;
        int w, h;
        cq_size(c, &w, &h);
        const char *p = q->cond;
        if (!cq_expr(&p, p + strlen(p), w, c->style->cq_type == CQ_SIZE ? h : -1))
            return 0;
    }
    return 1;
}

/* @container [name] condition: a query for the rules in its block. */
static const struct cquery *cq_parse(struct sheet *sh, const char *s, const char *e)
{
    struct cquery *q = arena_alloc(sh->a, sizeof(*q));
    if (!q)
        return sh->active_cq;
    while (s < e && isspace((unsigned char)*s))
        s++;
    while (e > s && isspace((unsigned char)e[-1]))
        e--;
    q->name = NULL;
    const char *k = s;
    while (k < e && is_ident_char(*k))
        k++;
    if (k > s && (k == e || *k != '(') && !word_at(s, e, "not") && !word_at(s, e, "style")) {
        char *name = arena_alloc(sh->a, (size_t)(k - s) + 1);
        if (name) {
            memcpy(name, s, (size_t)(k - s));
            name[k - s] = 0;
        }
        q->name = name;
        s = k;
    }
    q->cond = trimmed(sh->a, s, e);
    if (!q->cond)
        q->cond = "";
    q->outer = sh->active_cq;
    return q;
}

static int media_matches(const char *s, const char *end, int vw)
{
    char q[512];
    int n = (int)(end - s) < 511 ? (int)(end - s) : 511;
    for (int i = 0; i < n; i++)
        q[i] = (char)tolower((unsigned char)s[i]);
    q[n] = 0;
    if (!q[strspn(q, " \t\r\n")])
        return 1;
    char *save, *query = strtok_r(q, ",", &save);
    for (; query; query = strtok_r(NULL, ",", &save)) {
        while (isspace((unsigned char)*query))
            query++;
        int negate = 0;
        if (!strncmp(query, "not ", 4))
            negate = 1, query += 4;
        else if (!strncmp(query, "only ", 5))
            query += 5;
        while (isspace((unsigned char)*query))
            query++;
        int ok = 1;
        if (!strncmp(query, "print", 5) || !strncmp(query, "speech", 6) ||
            !strncmp(query, "tv", 2) || !strncmp(query, "handheld", 8))
            ok = 0;
        /* features joined by "and" (all must match) or "or" (one must) */
        int any = 0, first = 1, use_or = strstr(query, " or ") != NULL;
        for (char *f = strchr(query, '('); f && (use_or || ok); f = strchr(f + 1, '(')) {
            int r;
            if (f > query && !strncmp(f - 4 > query ? f - 4 : query, "not ", 4))
                r = !media_feature(f, vw);
            else
                r = media_feature(f, vw);
            if (use_or)
                any |= r;
            else
                ok = r;
            first = 0;
            char *close = strchr(f, ')');
            if (!close)
                break;
            f = close;
        }
        if (use_or && !first)
            ok = ok && any;
        if (negate)
            ok = !ok;
        if (ok)
            return 1;
    }
    return 0;
}

/* The URLs a style sheet's @import rules name (those whose media match), one after another:
 * calls found(url) for each; returns how many. */
int css_imports(const char *text, int len, int vw, void (*found)(void *ctx, const char *url), void *ctx)
{
    const char *p = text, *end = text + len;
    int n = 0;
    while (p < end) {
        struct scan sc = { p, end };
        skip_ws(&sc);
        p = sc.s;
        if (p >= end || *p != '@')
            break;                      /* @import must come first (after @charset and @layer) */
        const char *kw = p + 1, *k = kw;
        while (k < end && is_ident_char(*k))
            k++;
        const char *semi = find_top(k, end, ';'), *brace = find_top(k, end, '{');
        if (brace < semi)
            break;                      /* a block (@media, @layer {...}): the imports are over */
        if (k - kw == 6 && !strncasecmp(kw, "import", 6)) {
            const char *q = k;
            while (q < semi && isspace((unsigned char)*q))
                q++;
            if (q + 4 <= semi && !strncasecmp(q, "url(", 4))
                q += 4;
            while (q < semi && isspace((unsigned char)*q))
                q++;
            char quote = q < semi && (*q == '"' || *q == '\'') ? *q++ : 0;
            const char *u = q;
            while (q < semi && (quote ? *q != quote : *q != ')' && !isspace((unsigned char)*q)))
                q++;
            char url[URL_MAX];
            snprintf(url, sizeof(url), "%.*s", (int)(q - u), u);
            if (q < semi)
                q++;
            while (q < semi && (*q == ')' || isspace((unsigned char)*q)))
                q++;
            /* what follows: layer(...), supports(...), a media query list */
            while (q < semi && (!strncasecmp(q, "layer", 5) || !strncasecmp(q, "supports(", 9))) {
                const char *paren = q;
                while (paren < semi && *paren != '(' && !isspace((unsigned char)*paren))
                    paren++;
                q = paren < semi && *paren == '(' ? skip_group(paren, semi) : paren;
                while (q < semi && isspace((unsigned char)*q))
                    q++;
            }
            if (url[0] && media_matches(q, semi, vw)) {
                found(ctx, url);
                n++;
            }
        }
        p = semi < end ? semi + 1 : end;
    }
    return n;
}

int css_media_query(const char *query, int vw, int vh)
{
    int saved = css_viewport_h;
    css_viewport_h = vh;
    int r = media_matches(query, query + strlen(query), vw);
    css_viewport_h = saved;
    return r;
}

/* ---- sheets ---- */

static void add_rule(struct sheet *sh, const char *sel_s, const char *sel_e, const char *body_s,
                     const char *body_e)
{
    struct arena *a = sh->a;
    struct rule r = { 0 };
    r.layer = sh->active_layer;
    r.cq = sh->active_cq;
    r.starting = sh->active_starting > 0;
    r.ndecls = parse_decls(a, body_s, body_e, &r.decls);
    if (!r.ndecls)
        return;
    int max = 1;
    for (const char *s = sel_s; (s = find_top(s, sel_e, ',')) < sel_e; s++)
        max++;
    r.sels = arena_alloc(a, sizeof(struct selector) * (size_t)max);
    if (!r.sels)
        return;
    const char *s = sel_s;
    while (s < sel_e && r.nsels < max) {
        const char *comma = find_top(s, sel_e, ',');
        if (parse_selector(a, s, comma, &r.sels[r.nsels]) == 0)
            r.nsels++;
        s = comma < sel_e ? comma + 1 : sel_e;
    }
    if (!r.nsels)
        return;
    css_starting_rules += r.starting;
    if (sh->nrules == sh->cap) {
        int cap = sh->cap ? sh->cap * 2 : 64;
        struct rule *nr = arena_alloc(a, sizeof(struct rule) * (size_t)cap);
        if (!nr)
            return;
        if (sh->nrules)
            memcpy(nr, sh->rules, sizeof(struct rule) * (size_t)sh->nrules);
        sh->rules = nr;
        sh->cap = cap;
    }
    sh->rules[sh->nrules++] = r;
}

/* ---- @supports ---- */

static int is_length(const char *s);
static int supports_expr(struct arena *a, const char **pp, const char *e);

static int word_at(const char *p, const char *e, const char *w)
{
    size_t n = strlen(w);
    return (size_t)(e - p) > n && !strncasecmp(p, w, n) && (isspace((unsigned char)p[n]) || p[n] == '(');
}

/* (property: value): a property we know, with a value it can take. */
static int transform_supported(const char *value);
static int individual_supported(int prop, const char *value);
static int effect_supported(int prop, const char *value);
static int subgrid_supported(const char *value);
static int perspective_supported(const char *value);

static int supports_decl(const char *ns, const char *ne, const char *vs, const char *ve)
{
    while (ns < ne && isspace((unsigned char)*ns))
        ns++;
    while (ne > ns && isspace((unsigned char)ne[-1]))
        ne--;
    while (vs < ve && isspace((unsigned char)*vs))
        vs++;
    while (ve > vs && isspace((unsigned char)ve[-1]))
        ve--;
    char name[64], value[256];
    if (ne - ns <= 0 || ne - ns >= (int)sizeof(name) || ve - vs >= (int)sizeof(value))
        return 0;
    if (ne - ns > 2 && ns[0] == '-' && ns[1] == '-')
        return 1;                       /* custom properties take anything */
    for (int i = 0; i < ne - ns; i++)
        name[i] = (char)tolower((unsigned char)ns[i]);
    name[ne - ns] = 0;
    snprintf(value, sizeof(value), "%.*s", (int)(ve - vs), vs);
    int prop = prop_lookup(name);
    if (prop == P_UNKNOWN || prop == P_IGNORED || !value[0])
        return 0;
    if (!strncasecmp(value, "var(", 4) || !strcasecmp(value, "inherit") || !strcasecmp(value, "initial") ||
        !strcasecmp(value, "unset") || !strcasecmp(value, "revert"))
        return 1;
    uint32_t c;
    switch (prop) {
    case P_COLOR: case P_BACKGROUND_COLOR: case P_BORDER_COLOR: case P_BORDER_TOP_COLOR:
    case P_BORDER_RIGHT_COLOR: case P_BORDER_BOTTOM_COLOR: case P_BORDER_LEFT_COLOR:
        return css_parse_color(value, (int)strlen(value), &c) == 0 || !strcasecmp(value, "currentcolor");
    case P_DISPLAY: {
        static const char *const ok[] = { "none", "block", "inline", "inline-block", "flex", "inline-flex", "grid",
            "inline-grid", "table", "inline-table", "table-row", "table-cell", "table-row-group",
            "table-header-group", "table-footer-group", "table-caption", "table-column", "table-column-group",
            "list-item", "contents", "flow-root", "run-in", "-webkit-box", "-webkit-flex", "-ms-flexbox", "ruby",
            "ruby-text", "ruby-base", "ruby-base-container", "ruby-text-container", "block flex", "inline flex",
            "block grid", "inline grid", "block flow", "inline flow", "block flow-root", "inline flow-root",
            "inline list-item", NULL };
        for (int i = 0; ok[i]; i++)
            if (!strcasecmp(value, ok[i]))
                return 1;
        return 0;
    }
    case P_GRID_TEMPLATE_COLUMNS: case P_GRID_TEMPLATE_ROWS:
        if(!strncasecmp(value,"subgrid",7))return subgrid_supported(value);
        return 1;
    case P_GRID_AUTO_ROWS: case P_GRID_AUTO_COLUMNS:
        return strncasecmp(value,"subgrid",7)!=0;
    case P_PERSPECTIVE:return perspective_supported(value);
    case P_TRANSFORM: return transform_supported(value);
    case P_FILTER: case P_BACKDROP: case P_CLIP_PATH: return effect_supported(prop, value);
    case P_TRANSLATE: case P_ROTATE: case P_SCALE: return individual_supported(prop, value);
    case P_POSITION:
        return !strcasecmp(value, "static") || !strcasecmp(value, "relative") || !strcasecmp(value, "absolute") ||
               !strcasecmp(value, "fixed") || !strcasecmp(value, "sticky") || !strcasecmp(value, "-webkit-sticky");
    case P_WIDTH: case P_HEIGHT: case P_MIN_WIDTH: case P_MAX_WIDTH: case P_MIN_HEIGHT: case P_MAX_HEIGHT:
    case P_TOP: case P_RIGHT: case P_BOTTOM: case P_LEFT: case P_GAP: case P_ROW_GAP: case P_COLUMN_GAP:
        return is_length(value) || !strcasecmp(value, "fit-content") || !strcasecmp(value, "min-content") ||
               !strcasecmp(value, "max-content") || !strncasecmp(value, "anchor(", 7) ||
               !strncasecmp(value, "anchor-size(", 12);
    default:
        return 1;
    }
}

static int supports_term(struct arena *a, const char **pp, const char *e)
{
    const char *p = *pp;
    while (p < e && isspace((unsigned char)*p))
        p++;
    if (word_at(p, e, "not")) {
        *pp = p + 3;
        return !supports_term(a, pp, e);
    }
    if (e - p > 9 && !strncasecmp(p, "selector(", 9)) {
        const char *close = skip_group(p + 8, e);
        struct selector sel;
        *pp = close;
        return parse_selector(a, p + 9, close - 1, &sel) == 0;
    }
    if (p < e && *p == '(') {
        const char *close = skip_group(p, e), *in = p + 1, *ie = close - 1;
        *pp = close;
        while (in < ie && isspace((unsigned char)*in))
            in++;
        const char *colon = find_top(in, ie, ':');
        if (in < ie && (*in == '(' || word_at(in, ie, "not") || colon >= ie ||
                        (ie - in > 9 && !strncasecmp(in, "selector(", 9))))
            return supports_expr(a, &in, ie);
        return supports_decl(in, colon, colon + 1, ie);
    }
    /* a function this does not know (font-tech() and the like): false */
    while (p < e && *p != '(' && !isspace((unsigned char)*p))
        p++;
    *pp = p < e && *p == '(' ? skip_group(p, e) : e;
    return 0;
}

static int supports_expr(struct arena *a, const char **pp, const char *e)
{
    int r = supports_term(a, pp, e);
    for (;;) {
        const char *p = *pp;
        while (p < e && isspace((unsigned char)*p))
            p++;
        if (word_at(p, e, "and")) {
            *pp = p + 3;
            int t = supports_term(a, pp, e);
            r = r && t;
        } else if (word_at(p, e, "or")) {
            *pp = p + 2;
            int t = supports_term(a, pp, e);
            r = r || t;
        } else {
            *pp = p;
            return r;
        }
    }
}

static int supports_matches(struct arena *a, const char *s, const char *e)
{
    return supports_expr(a, &s, e);
}

int css_supports(const char *cond)
{
    struct arena a = { 0 };
    char buf[512];
    const char *c = cond;
    while (isspace((unsigned char)*c))
        c++;
    /* CSS.supports("display: flex") is a declaration without its parentheses */
    if (*c != '(' && strncasecmp(c, "not", 3) && strncasecmp(c, "selector(", 9) && strchr(c, ':'))
        snprintf(buf, sizeof(buf), "(%s)", c);
    else
        snprintf(buf, sizeof(buf), "%s", c);
    int r = supports_matches(&a, buf, buf + strlen(buf));
    arena_free(&a);
    return r;
}

/* ---- nesting ---- */

static struct layer *layer_name(struct sheet *sh, const char *s, const char *e)
{
    struct layer *parent = sh->active_layer;
    while (s < e && isspace((unsigned char)*s)) s++;
    while (e > s && isspace((unsigned char)e[-1])) e--;
    char anonymous[64];
    if (s == e) {
        snprintf(anonymous, sizeof(anonymous), "@%p:%u", (void *)sh, ++sh->anonymous_layer);
        s = anonymous;
        e = s + strlen(s);
    }
    while (s < e) {
        const char *dot = memchr(s, '.', (size_t)(e - s));
        const char *end = dot ? dot : e;
        if (end == s) return NULL;
        struct layer *l;
        for (l = sh->layers; l; l = l->next)
            if (l->parent == parent && strlen(l->name) == (size_t)(end - s) &&
                !strncmp(l->name, s, (size_t)(end - s))) break;
        if (!l) {
            l = arena_alloc(sh->a, sizeof(*l));
            if (!l) return NULL;
            l->name = arena_strndup(sh->a, s, (size_t)(end - s));
            l->parent = parent;
            if (sh->last_layer) sh->last_layer->next = l;
            else sh->layers = l;
            sh->last_layer = l;
        }
        parent = l;
        s = dot ? dot + 1 : e;
    }
    return parent;
}

static void style_rule(struct sheet *sh, const char *sel_s, const char *sel_e, const char *body_s,
                       const char *body_e, int vw, int depth);

/* Does a selector list hold just one compound (so & can stand for it as it is)? */
static int single_compound(const char *s, const char *e)
{
    for (const char *p = s; p < e; p++) {
        if (*p == '(' || *p == '[' || *p == '"' || *p == '\'') {
            p = skip_group(p, e) - 1;
            continue;
        }
        if (*p == ',' || *p == ' ' || *p == '>' || *p == '+' || *p == '~' || *p == '\t' || *p == '\n')
            return 0;
    }
    return 1;
}

/* A nested rule's selectors, with the parent's put in: "&:hover" is the parent
 * hovered; one without & is a descendant (or sibling, after + or ~) of it. */
static char *nest_selector(struct arena *a, const char *ps, const char *pe, const char *cs, const char *ce,
                           int *len)
{
    while (ps < pe && isspace((unsigned char)*ps))
        ps++;
    while (pe > ps && isspace((unsigned char)pe[-1]))
        pe--;
    int simple = single_compound(ps, pe), plen = (int)(pe - ps);
    int amps = 0, parts = 1;
    for (const char *p = cs; p < ce; p++)
        amps += *p == '&', parts += *p == ',';
    size_t cap = (size_t)(ce - cs) + (size_t)(amps + parts) * (size_t)(plen + 8) + 8;
    char *out = arena_alloc(a, cap);
    if (!out)
        return NULL;
    int o = 0;
#define PUT_PARENT() do { if (!simple) { memcpy(out + o, ":is(", 4); o += 4; }                           memcpy(out + o, ps, (size_t)plen); o += plen; if (!simple) out[o++] = ')'; } while (0)
    for (const char *s = cs; s < ce;) {
        const char *comma = find_top(s, ce, ',');
        const char *a0 = s, *a1 = comma;
        while (a0 < a1 && isspace((unsigned char)*a0))
            a0++;
        while (a1 > a0 && isspace((unsigned char)a1[-1]))
            a1--;
        if (a1 > a0) {
            if (o)
                out[o++] = ',';
            if (!memchr(a0, '&', (size_t)(a1 - a0))) {
                PUT_PARENT();
                out[o++] = ' ';
            }
            for (const char *p = a0; p < a1; p++) {
                if (*p == '&')
                    PUT_PARENT();
                else
                    out[o++] = *p;
            }
        }
        s = comma < ce ? comma + 1 : ce;
    }
#undef PUT_PARENT
    out[o] = 0;
    *len = o;
    return out;
}

static void style_rule(struct sheet *sh, const char *sel_s, const char *sel_e, const char *body_s,
                       const char *body_e, int vw, int depth);

/* @scope (root) [to (limit)] { rules }: the rules nested in the root's selector, :scope standing
 * for it. The limit is not kept: what is below it matches too. A scope with no root (the
 * parent of a <style>) cannot be told from here, and its rules are left out. */
static void scope_rule(struct sheet *sh, const char *pre_s, const char *pre_e, const char *body_s,
                       const char *body_e, int vw, int depth)
{
    while (pre_s < pre_e && isspace((unsigned char)*pre_s))
        pre_s++;
    if (pre_s >= pre_e || *pre_s != '(' || depth >= 8)
        return;
    const char *close = skip_group(pre_s, pre_e);
    size_t n = (size_t)(body_e - body_s);
    char *body = arena_alloc(sh->a, n + 1);
    if (!body || close - pre_s < 3)
        return;
    size_t o = 0;
    for (const char *p = body_s; p < body_e;) {
        if (body_e - p >= 6 && !strncasecmp(p, ":scope", 6) && !(body_e - p > 6 && is_ident_char(p[6]))) {
            body[o++] = '&';
            p += 6;
        } else {
            body[o++] = *p++;
        }
    }
    style_rule(sh, pre_s + 1, close - 1, body, body + o, vw, depth + 1);
}

/* A style rule. Its body may hold rules of its own (CSS nesting), and
 * @media and @supports blocks whose declarations are the rule's. */
static void style_rule(struct sheet *sh, const char *sel_s, const char *sel_e, const char *body_s,
                       const char *body_e, int vw, int depth)
{
    if (!memchr(body_s, '{', (size_t)(body_e - body_s)) || depth >= 8) {
        add_rule(sh, sel_s, sel_e, body_s, body_e);
        return;
    }
    /* the declarations first, then what is nested, in order */
    char *decls = arena_alloc(sh->a, (size_t)(body_e - body_s) + 2);
    int dn = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *p = body_s;
        while (p < body_e) {
            struct scan sc = { p, body_e };
            skip_ws(&sc);
            p = sc.s;
            if (p >= body_e)
                break;
            const char *semi = find_top(p, body_e, ';'), *brace = find_top(p, body_e, '{');
            if (brace >= semi) {                /* a declaration */
                if (!pass && decls) {
                    memcpy(decls + dn, p, (size_t)(semi - p));
                    dn += (int)(semi - p);
                    decls[dn++] = ';';
                }
                p = semi < body_e ? semi + 1 : body_e;
                continue;
            }
            const char *close = skip_group(brace, body_e);
            if (pass) {
                if (*p == '@') {
                    const char *kw = p + 1, *k = kw;
                    while (k < brace && is_ident_char(*k))
                        k++;
                    int kl = (int)(k - kw);
                    if (kl == 5 && !strncasecmp(kw, "layer", 5)) {
                        struct layer *saved = sh->active_layer;
                        sh->active_layer = layer_name(sh, k, brace);
                        style_rule(sh, sel_s, sel_e, brace + 1, close - 1, vw, depth + 1);
                        sh->active_layer = saved;
                    } else if (kl == 9 && !strncasecmp(kw, "container", 9)) {
                        const struct cquery *saved = sh->active_cq;
                        sh->active_cq = cq_parse(sh, k, brace);
                        style_rule(sh, sel_s, sel_e, brace + 1, close - 1, vw, depth + 1);
                        sh->active_cq = saved;
                    } else if (kl == 14 && !strncasecmp(kw, "starting-style", 14)) {
                        sh->active_starting++;
                        style_rule(sh, sel_s, sel_e, brace + 1, close - 1, vw, depth + 1);
                        sh->active_starting--;
                    } else if ((kl == 5 && !strncasecmp(kw, "media", 5) && media_matches(k, brace, vw)) ||
                        (kl == 8 && !strncasecmp(kw, "supports", 8) && supports_matches(sh->a, k, brace)))
                        style_rule(sh, sel_s, sel_e, brace + 1, close - 1, vw, depth + 1);
                } else {
                    int len;
                    char *sel = nest_selector(sh->a, sel_s, sel_e, p, brace, &len);
                    if (sel)
                        style_rule(sh, sel, sel + len, brace + 1, close - 1, vw, depth + 1);
                }
            }
            p = close;
        }
        if (!pass && decls && dn)
            add_rule(sh, sel_s, sel_e, decls, decls + dn);
    }
}

static void parse_block(struct sheet *sh, const char *s, const char *end, int vw, int depth)
{
    struct scan sc = { s, end };
    while (sc.s < sc.end) {
        skip_ws(&sc);
        if (sc.s >= sc.end)
            break;
        if (*sc.s == '}' || *sc.s == ';') {
            sc.s++;
            continue;
        }
        if (*sc.s == '@') {
            const char *kw = sc.s + 1, *k = kw;
            while (k < sc.end && is_ident_char(*k))
                k++;
            int kl = (int)(k - kw);
            const char *brace = find_top(k, sc.end, '{'), *semi = find_top(k, sc.end, ';');
            if (semi < brace) {         /* @import, @charset, @namespace, @layer a, b; */
                if (kl == 5 && !strncasecmp(kw, "layer", 5))
                    for (const char *p = k; p < semi;) {
                        const char *comma = find_top(p, semi, ',');
                        layer_name(sh, p, comma);
                        p = comma < semi ? comma + 1 : semi;
                    }
                sc.s = semi + 1;
                continue;
            }
            if (brace >= sc.end)
                break;
            const char *close = skip_group(brace, sc.end);
            if ((kl == 9 && !strncasecmp(kw, "keyframes", 9)) ||
                (kl == 17 && !strncasecmp(kw, "-webkit-keyframes", 17)) ||
                (kl == 14 && !strncasecmp(kw, "-moz-keyframes", 14))) {
                parse_keyframes(sh, k, brace, brace + 1, close - 1);
                sc.s = close;
                continue;
            }
            if (depth < 8 && kl == 5 && !strncasecmp(kw, "layer", 5)) {
                struct layer *saved = sh->active_layer;
                sh->active_layer = layer_name(sh, k, brace);
                parse_block(sh, brace + 1, close - 1, vw, depth + 1);
                sh->active_layer = saved;
                sc.s = close;
                continue;
            }
            if (depth < 8 && kl == 9 && !strncasecmp(kw, "container", 9)) {
                const struct cquery *saved = sh->active_cq;
                sh->active_cq = cq_parse(sh, k, brace);
                parse_block(sh, brace + 1, close - 1, vw, depth + 1);
                sh->active_cq = saved;
                sc.s = close;
                continue;
            }
            if (depth < 8 && kl == 14 && !strncasecmp(kw, "starting-style", 14)) {
                sh->active_starting++;
                parse_block(sh, brace + 1, close - 1, vw, depth + 1);
                sh->active_starting--;
                sc.s = close;
                continue;
            }
            if (kl == 8 && !strncasecmp(kw, "property", 8)) {      /* @property --x { initial-value: ... } */
                char *name = trimmed(sh->a, k, brace);
                for (const char *p = brace + 1; name && name[0] == '-' && name[1] == '-' && p < close - 1;) {
                    const char *semi = find_top(p, close - 1, ';'), *colon = find_top(p, semi, ':');
                    while (p < colon && isspace((unsigned char)*p))
                        p++;
                    struct custom_prop *cp;
                    if (colon < semi && colon - p >= 13 && !strncasecmp(p, "initial-value", 13) &&
                        (cp = arena_alloc(sh->a, sizeof(*cp)))) {
                        cp->name = name;
                        cp->value = trimmed(sh->a, colon + 1, semi);
                        cp->next = sh->registered;
                        sh->registered = cp->value ? cp : sh->registered;
                    }
                    p = semi + 1;
                }
                sc.s = close;
                continue;
            }
            if (kl == 5 && !strncasecmp(kw, "scope", 5)) {
                scope_rule(sh, k, brace, brace + 1, close - 1, vw, depth);
                sc.s = close;
                continue;
            }
            if (depth < 8 && ((kl == 5 && !strncasecmp(kw, "media", 5) && media_matches(k, brace, vw)) ||
                              (kl == 8 && !strncasecmp(kw, "supports", 8) && supports_matches(sh->a, k, brace)) ||
                              (kl == 8 && !strncasecmp(kw, "document", 8))))
                parse_block(sh, brace + 1, close - 1, vw, depth + 1);
            sc.s = close;
            continue;
        }
        const char *brace = find_top(sc.s, sc.end, '{');
        if (brace >= sc.end)
            break;
        const char *close = skip_group(brace, sc.end);
        style_rule(sh, sc.s, brace, brace + 1, close - 1, vw, 0);
        sc.s = close;
    }
}

static uint32_t hash_n(const char *s, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ (uint8_t)s[i]) * 16777619u;
    return h;
}

static uint32_t hash_str(const char *s)
{
    return hash_n(s, strlen(s));
}

/*
 * ---- the ancestor filter ----
 *
 * While the cascade walks the tree it keeps a counting Bloom filter of
 * the ids, classes and tags of the element's ancestors. A selector such
 * as ".nav ul li a" is filed under "a" and so tried on every link; but
 * it cannot match a link with no ".nav" above it, and the filter says so
 * without walking up. Each selector carries the keys of the compounds
 * that must be ancestors of its subject: those followed by a descendant
 * or child combinator (a sibling's ancestors are the subject's too).
 */
enum { KEY_TAG = 1, KEY_ID, KEY_CLASS };
#define BLOOM_BITS 12
#define BLOOM_SIZE (1u << BLOOM_BITS)

struct bloom {
    uint16_t count[BLOOM_SIZE];
    int broken;                 /* an element's keys could not be read: it would miss them */
};

static uint32_t bloom_key(int kind, uint32_t h)
{
    uint32_t k = (h ^ (uint32_t)kind * 0x9E3779B1u) * 0x85EBCA6Bu;
    return k ^ (k >> 15);
}

static int bloom_has(const struct bloom *b, uint32_t k)
{
    return b->count[k & (BLOOM_SIZE - 1)] && b->count[(k >> BLOOM_BITS) & (BLOOM_SIZE - 1)];
}

static void bloom_adjust(struct bloom *b, uint32_t k, int d)
{
    uint16_t *c1 = &b->count[k & (BLOOM_SIZE - 1)], *c2 = &b->count[(k >> BLOOM_BITS) & (BLOOM_SIZE - 1)];
    *c1 = (uint16_t)(*c1 + d);
    *c2 = (uint16_t)(*c2 + d);
}

/* The keys of a selector's ancestor compounds, most selective first (ids, classes, then tags). */
static void selector_ancestor_keys(struct selector *sel)
{
    sel->nanc = 0;
    for (int i = 0; i < sel->nparts; i++)
        if (sel->parts[i].host || sel->parts[i].nslot)
            return;                     /* shadow scopes: ancestors cross the host */
    for (int pass = 0; pass < 3; pass++)
        for (int i = sel->nparts - 2; i >= 0 && sel->nanc < 4; i--) {
            const struct compound *c = &sel->parts[i];
            char comb = sel->parts[i + 1].comb;
            if (comb != ' ' && comb != '>' && comb != 0)
                continue;               /* a sibling, not an ancestor */
            if (pass == 0 && c->id)
                sel->anc[sel->nanc++] = bloom_key(KEY_ID, c->id_h);
            for (int k = 0; pass == 1 && k < c->nclasses && sel->nanc < 4; k++)
                sel->anc[sel->nanc++] = bloom_key(KEY_CLASS, c->class_h[k]);
            if (pass == 2 && c->tag > 0 && c->tag != T_UNKNOWN)
                sel->anc[sel->nanc++] = bloom_key(KEY_TAG, (uint32_t)c->tag);
        }
}

/*
 * ---- an element's id and classes, read once per cascade ----
 *
 * Matching asks for them thousands of times per element; parsing the
 * class attribute again each time was most of the cascade's work.
 */
struct nclass {
    uint32_t h;
    const char *p;
    uint32_t len;
};

struct nkeys {
    const char *id;
    uint32_t id_h;
    int ncls;
    struct nclass cls[];
};

static unsigned key_gen = 1;            /* bumped by every cascade: older keys are stale */
static struct arena *key_arena;

static const struct nkeys *node_keys(const struct node *cn)
{
    struct node *n = (struct node *)cn;
    if (n->kgen == key_gen && n->keys)
        return n->keys;
    const char *id = NULL, *cls = NULL;
    for (const struct attr *a = n->attrs; a; a = a->next) {
        if (!id && !strcmp(a->name, "id"))
            id = a->value;
        else if (!cls && !strcmp(a->name, "class"))
            cls = a->value;
    }
    int count = 0;
    for (const char *p = cls; p && *p;) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (*p)
            count++;
        while (*p && !isspace((unsigned char)*p))
            p++;
    }
    struct nkeys *k = key_arena ? arena_alloc(key_arena, sizeof(*k) + sizeof(struct nclass) * (size_t)count) : NULL;
    if (!k)
        return NULL;
    k->id = id;
    k->id_h = id ? hash_str(id) : 0;
    for (const char *p = cls; p && *p;) {
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *e = p;
        while (*e && !isspace((unsigned char)*e))
            e++;
        if (e > p) {
            k->cls[k->ncls].p = p;
            k->cls[k->ncls].len = (uint32_t)(e - p);
            k->cls[k->ncls].h = hash_n(p, (size_t)(e - p));
            k->ncls++;
        }
        p = e;
    }
    n->keys = k;
    n->kgen = key_gen;
    return k;
}

/* Put an element's keys into the filter (d 1) or take them out (d -1). */
static void bloom_node(struct bloom *b, const struct node *n, int d)
{
    if (!b || n->type != N_ELEMENT)
        return;
    if (n->tag > 0 && n->tag != T_UNKNOWN)
        bloom_adjust(b, bloom_key(KEY_TAG, n->tag), d);
    const struct nkeys *k = node_keys(n);
    if (!k) {
        if (d > 0)
            b->broken = 1;
        return;
    }
    if (k->id)
        bloom_adjust(b, bloom_key(KEY_ID, k->id_h), d);
    for (int i = 0; i < k->ncls; i++)
        bloom_adjust(b, bloom_key(KEY_CLASS, k->cls[i].h), d);
}

static void index_sheet(struct sheet *sh)
{
    for (int r = sh->nrules - 1; r >= 0; r--) {
        for (int i = sh->rules[r].nsels - 1; i >= 0; i--) {
            struct selector *sel = &sh->rules[r].sels[i];
            selector_ancestor_keys(sel);
            const struct compound *c = &sel->parts[sel->nparts - 1];
            struct ref *ref = arena_alloc(sh->a, sizeof(*ref));
            if (!ref)
                return;
            ref->rule = r;
            ref->sel = i;
            struct ref **list;
            if (c->id)
                list = &sh->by_id[hash_str(c->id) % INDEX_BUCKETS];
            else if (c->nclasses)
                list = &sh->by_class[hash_str(c->classes[0]) % INDEX_BUCKETS];
            else if (c->tag > 0)
                list = &sh->by_tag[c->tag];
            else
                list = &sh->other;
            ref->next = *list;
            *list = ref;
        }
    }
}

struct sheet *css_parse(struct arena *a, const char *text, int len, int viewport_w)
{
    struct sheet *sh = arena_alloc(a, sizeof(*sh));
    if (!sh)
        return NULL;
    sh->a = a;
    parse_block(sh, text, text + len, viewport_w, 0);
    index_sheet(sh);
    return sh;
}

/* ---- matching ---- */

const struct node *css_hover_node, *css_focus_node, *css_active_node, *css_focus_visible_node;
int css_dynamic_rules;

struct mctx {
    int (*visited)(const char *href, void *ctx);
    void *ctx;
    int quirks;
    const struct node *host, *slotted;
};

static const struct node *prev_element(const struct node *n)
{
    for (n = n->prev; n && n->type != N_ELEMENT; n = n->prev)
        ;
    return n;
}

static const struct node *next_element(const struct node *n)
{
    for (n = n->next; n && n->type != N_ELEMENT; n = n->next)
        ;
    return n;
}

/*
 * What the cascade is matching for: the element whose style is being
 * computed, the filter of its ancestors (NULL where it does not apply),
 * and what its style turns out to depend on (node->style_flags):
 *   SF_NOSHARE   a test of the element itself beyond its tag and
 *                attributes (position, state, children...): a sibling
 *                with the same attributes may not take its style;
 *   SF_DYN_SELF  its own :hover, :active, :focus... state;
 *   SF_DYN_OTHER such a state of some other element (".a:hover .b").
 */
#define SF_NOSHARE   1
#define SF_DYN_SELF  2
#define SF_DYN_OTHER 4
#define PS_DYNAMIC   (PS_HOVER | PS_ACTIVE | PS_FOCUS | PS_FOCUS_WITHIN | PS_USER_INVALID)

static const struct node *cur_subject;
static struct bloom *cur_bloom;
static uint8_t cur_flags;

static int has_class(const struct node *n, const char *cls)
{
    const char *v = node_attr(n, "class");
    if (!v)
        return 0;
    size_t len = strlen(cls);
    for (const char *p = v; *p;) {
        while (*p && isspace((unsigned char)*p))
            p++;
        const char *e = p;
        while (*e && !isspace((unsigned char)*e))
            e++;
        if ((size_t)(e - p) == len && !strncmp(p, cls, len))
            return 1;
        p = e;
    }
    return 0;
}

static int same_type(const struct node *a, const struct node *b)
{
    return a->tag == b->tag && (a->tag != T_UNKNOWN || !strcmp(a->name, b->name));
}

static int match_attr(const struct node *n, const struct attr_sel *at)
{
    const char *v = node_attr(n, at->name);
    if (!v)
        return 0;
    if (!at->op)
        return 1;
    int (*cmp)(const char *, const char *, size_t) = at->nocase ? strncasecmp : strncmp;
    size_t vl = strlen(v), al = strlen(at->value);
    switch (at->op) {
    case '=':
        return vl == al && !cmp(v, at->value, al);
    case '^':
        return al && vl >= al && !cmp(v, at->value, al);
    case '$':
        return al && vl >= al && !cmp(v + vl - al, at->value, al);
    case '*':
        if (!al)
            return 0;
        for (size_t i = 0; i + al <= vl; i++)
            if (!cmp(v + i, at->value, al))
                return 1;
        return 0;
    case '|':
        return (vl == al || (vl > al && v[al] == '-')) && !cmp(v, at->value, al);
    case '~':
        for (const char *p = v; *p;) {
            while (*p == ' ')
                p++;
            const char *e = p;
            while (*e && *e != ' ')
                e++;
            if ((size_t)(e - p) == al && !cmp(p, at->value, al))
                return 1;
            p = e;
        }
        return 0;
    }
    return 0;
}

static int is_link(const struct node *n)
{
    return (n->tag == T_A || n->tag == T_AREA) && node_attr(n, "href");
}

static int match_from(const struct selector *s, int i, const struct node *n, const struct mctx *m);

/* ---- form state for :valid, :read-only, :default and friends ---- */

static const char *input_kind(const struct node *n)
{
    const char *t = node_attr(n, "type");
    return t ? t : "text";
}

/* A text-like field the user can type in: text, search, tel, url, email, password, number... */
static int is_typing_field(const struct node *n)
{
    if (n->tag == T_TEXTAREA)
        return 1;
    if (n->tag != T_INPUT)
        return 0;
    const char *t = input_kind(n);
    static const char *const other[] = {
        "hidden", "checkbox", "radio", "submit", "reset", "button", "image", "file", "range", "color",
    };
    for (size_t i = 0; i < sizeof(other) / sizeof(other[0]); i++)
        if (!strcasecmp(t, other[i]))
            return 0;
    return 1;
}

static int read_write(const struct node *n)
{
    if (node_attr(n, "disabled") || node_attr(n, "readonly"))
        return 0;
    return is_typing_field(n);
}

/* The control's current value: what was typed, else the attribute. */
static const char *field_value(const struct node *n)
{
    if (n->value)
        return n->value;
    if (n->tag == T_TEXTAREA)
        return n->first && n->first->type == N_TEXT ? n->first->text : "";
    const char *v = node_attr(n, "value");
    return v ? v : "";
}

static int checked_state(const struct node *n)
{
    return n->value ? n->checked : node_attr(n, "checked") != NULL;
}

/* The chosen option of a <select>: the one asked for, else the first that says selected, else the first. */
static const struct node *chosen_option(const struct node *sel)
{
    int k = 0, want = sel->value || sel->checked ? sel->checked : -1;
    const struct node *first = NULL, *selected = NULL;
    for (const struct node *o = sel->first; o; o = node_next((struct node *)o, sel)) {
        if (o->type != N_ELEMENT || o->tag != T_OPTION)
            continue;
        if (!first)
            first = o;
        if (!selected && node_attr(o, "selected"))
            selected = o;
        if (k++ == want)
            return o;
    }
    return selected ? selected : first;
}

static int is_number(const char *s, long *out)
{
    char *e;
    long v = strtol(s, &e, 10);
    if (e == s)
        return 0;
    while (*e == '.' || (*e >= '0' && *e <= '9'))
        e++;                            /* fractions: whole parts are enough for min and max */
    if (*e)
        return 0;
    *out = v;
    return 1;
}

/* 1 if the control passes its constraints, 0 if not; -1 for one that has none to check. */
static int control_validity(const struct node *n)
{
    if (n->tag == T_SELECT) {
        if (node_attr(n, "disabled"))
            return -1;
        if (node_attr(n, "required")) {
            const struct node *o = chosen_option(n);
            const char *v = o ? (node_attr(o, "value") ? node_attr(o, "value") : o->value ? o->value : "") : "";
            if (!o || !*v)
                return 0;
        }
        return 1;
    }
    if (n->tag != T_INPUT && n->tag != T_TEXTAREA)
        return -1;
    const char *t = n->tag == T_INPUT ? input_kind(n) : "textarea";
    if (node_attr(n, "disabled") || (is_typing_field(n) && node_attr(n, "readonly")))
        return -1;
    if (!strcasecmp(t, "hidden") || !strcasecmp(t, "reset") || !strcasecmp(t, "button") ||
        !strcasecmp(t, "image") || !strcasecmp(t, "submit"))
        return -1;
    int required = node_attr(n, "required") != NULL;
    if (!strcasecmp(t, "checkbox"))
        return required && !checked_state(n) ? 0 : 1;
    if (!strcasecmp(t, "radio")) {
        if (!required)
            return 1;
        const char *name = node_attr(n, "name");
        if (!name || !*name)
            return checked_state(n);
        const struct node *form = n;
        while (form->parent && form->tag != T_FORM)
            form = form->parent;
        for (const struct node *o = form; o; o = node_next((struct node *)o, form))
            if (o->type == N_ELEMENT && o->tag == T_INPUT && !strcasecmp(input_kind(o), "radio") &&
                node_attr(o, "name") && !strcmp(node_attr(o, "name"), name) && checked_state(o))
                return 1;
        return 0;
    }
    if (!strcasecmp(t, "file"))
        return required ? 0 : 1;
    const char *v = field_value(n);
    if (!*v)
        return required ? 0 : 1;
    if (!strcasecmp(t, "email")) {
        const char *at = strchr(v, '@');
        if (!at || at == v || !at[1] || strchr(at + 1, '@') || strpbrk(v, " \t"))
            return 0;
    } else if (!strcasecmp(t, "url")) {
        const char *colon = strstr(v, ":");
        if (!colon || colon == v || !colon[1] || strpbrk(v, " \t"))
            return 0;
    } else if (!strcasecmp(t, "number") || !strcasecmp(t, "range")) {
        long x, lo, hi;
        if (!is_number(v, &x))
            return 0;
        const char *mn = node_attr(n, "min"), *mx = node_attr(n, "max");
        if ((mn && is_number(mn, &lo) && x < lo) || (mx && is_number(mx, &hi) && x > hi))
            return 0;
    }
    int len = 0;
    for (const char *p = v; *p; p++)
        len += ((unsigned char)*p & 0xC0) != 0x80;
    const char *ml = node_attr(n, "minlength"), *xl = node_attr(n, "maxlength");
    if (xl && atoi(xl) > 0 && len > atoi(xl))
        return 0;
    if (ml && atoi(ml) > 0 && len < atoi(ml))
        return 0;
    return 1;
}

/* A form or fieldset is valid when every control in it is. */
static int tree_valid(const struct node *n)
{
    for (const struct node *k = n->first; k; k = node_next((struct node *)k, n))
        if (k->type == N_ELEMENT && control_validity(k) == 0)
            return 0;
    return 1;
}

static int match_valid(const struct node *n, int want_valid)
{
    int v;
    if (n->tag == T_FORM || n->tag == T_FIELDSET)
        v = tree_valid(n);
    else if ((v = control_validity(n)) < 0)
        return 0;
    return v ? want_valid : !want_valid;
}

/* The nearest lang attribute up the tree, or NULL. */
static const char *lang_of(const struct node *n)
{
    for (; n; n = n->parent) {
        const char *l = n->type == N_ELEMENT ? node_attr(n, "lang") : NULL;
        if (l)
            return l;
    }
    return NULL;
}

/* The default button of a form: the first submit button in it. */
static int is_default_button(const struct node *n)
{
    int submit = n->tag == T_BUTTON ? (!node_attr(n, "type") || !strcasecmp(node_attr(n, "type"), "submit"))
               : n->tag == T_INPUT && (!strcasecmp(input_kind(n), "submit") || !strcasecmp(input_kind(n), "image"));
    if (!submit)
        return 0;
    const struct node *form = n->parent;
    while (form && form->tag != T_FORM)
        form = form->parent;
    if (!form)
        return 0;
    for (const struct node *o = form->first; o; o = node_next((struct node *)o, form)) {
        if (o->type != N_ELEMENT)
            continue;
        int sub = o->tag == T_BUTTON ? (!node_attr(o, "type") || !strcasecmp(node_attr(o, "type"), "submit"))
                : o->tag == T_INPUT && (!strcasecmp(input_kind(o), "submit") || !strcasecmp(input_kind(o), "image"));
        if (sub)
            return o == n;
    }
    return 0;
}

static int match_nth_filter(const struct compound *c, const struct node *n, const struct mctx *m)
{
    if (!c->nnth)
        return 1;
    for (int i = 0; i < c->nnth; i++)
        if (match_from(&c->nth_list[i], c->nth_list[i].nparts - 1, n, m))
            return 1;
    return 0;
}

static int match_compound(const struct compound *c, const struct node *n, const struct mctx *m)
{
    if (c->host) {
        if (!m->host || n != m->host) return 0;
        if (c->nhost) {
            struct mctx outer = *m;
            outer.host = NULL;
            if (!match_from(c->host_args, 0, n, &outer)) return 0;
        }
    } else if (m->host && n == m->host) return 0;
    if (c->tag >= 0 && (c->tag != n->tag || (c->tag == T_UNKNOWN && strcmp(c->tagname, n->name))))
        return 0;
    const struct nkeys *k = (c->id || c->nclasses) ? node_keys(n) : NULL;
    if (c->id) {
        const char *id = k ? k->id : node_attr(n, "id");
        if (!id || (k && k->id_h != c->id_h) || strcmp(id, c->id))
            return 0;
    }
    for (int i = 0; i < c->nclasses; i++) {
        if (!k) {
            if (!has_class(n, c->classes[i]))
                return 0;
            continue;
        }
        int found = 0;
        for (int j = 0; j < k->ncls && !found; j++)
            found = k->cls[j].h == c->class_h[i] && !strncmp(k->cls[j].p, c->classes[i], k->cls[j].len) &&
                    !c->classes[i][k->cls[j].len];
        if (!found)
            return 0;
    }
    for (int i = 0; i < c->nattrs; i++)
        if (!match_attr(n, &c->attrs[i]))
            return 0;
    uint32_t ps = c->pseudo;
    if (n == cur_subject && (ps || c->focus_visible || c->has_nth || c->nhas || c->lang || c->state))
        cur_flags |= SF_NOSHARE;
    if (c->state) {
        if ((c->state & ST_MODAL) && !n->modal)
            return 0;
        if ((c->state & ST_POPOVER_OPEN) && !n->popover_open)
            return 0;
        if ((c->state & ST_OPEN) && !((n->tag == T_DETAILS || n->tag == T_DIALOG) && node_attr(n, "open")))
            return 0;
    }
    if ((ps & PS_DYNAMIC) || c->focus_visible)
        cur_flags |= n == cur_subject ? SF_DYN_SELF : SF_DYN_OTHER;
    if (ps) {
        if (ps & PS_NEVER)
            return 0;
        if ((ps & (PS_FIRST_CHILD | PS_ONLY_CHILD)) && prev_element(n))
            return 0;
        if ((ps & (PS_LAST_CHILD | PS_ONLY_CHILD)) && next_element(n))
            return 0;
        if (ps & PS_ONLY_OF_TYPE) {
            for (const struct node *p = prev_element(n); p; p = prev_element(p))
                if (same_type(p, n))
                    return 0;
            for (const struct node *p = next_element(n); p; p = next_element(p))
                if (same_type(p, n))
                    return 0;
        }
        if ((ps & PS_ANY_LINK) && !is_link(n))
            return 0;
        if ((ps & PS_REQUIRED) && !node_attr(n, "required"))
            return 0;
        if ((ps & PS_OPTIONAL) && (node_attr(n, "required") ||
                                   !(n->tag == T_INPUT || n->tag == T_SELECT || n->tag == T_TEXTAREA)))
            return 0;
        if ((ps & PS_PLACEHOLDER) && (!node_attr(n, "placeholder") || (n->value && *n->value) ||
                                      node_attr(n, "value")))
            return 0;
        if (ps & PS_FIRST_OF_TYPE)
            for (const struct node *p = prev_element(n); p; p = prev_element(p))
                if (same_type(p, n))
                    return 0;
        if (ps & PS_LAST_OF_TYPE)
            for (const struct node *p = next_element(n); p; p = next_element(p))
                if (same_type(p, n))
                    return 0;
        if (ps & PS_HOVER) {
            const struct node *h = css_hover_node;
            while (h && h != n)
                h = h->parent;
            if (!h)
                return 0;
        }
        if (ps & PS_ACTIVE) {
            const struct node *h = css_active_node;
            while (h && h != n)
                h = h->parent;
            if (!h)
                return 0;
        }
        if ((ps & PS_USER_INVALID) && (!match_valid(n, 0) || !((n->value && *n->value) || n == css_focus_node)))
            return 0;                   /* invalid, and the user has typed in it */
        if (c->focus_visible && n != css_focus_visible_node)
            return 0;
        if ((ps & PS_FOCUS) && n != css_focus_node)
            return 0;
        if (ps & PS_FOCUS_WITHIN) {
            const struct node *h = css_focus_node;
            while (h && h != n)
                h = h->parent;
            if (!h)
                return 0;
        }
        if (ps & (PS_LINK | PS_VISITED)) {
            if (!is_link(n))
                return 0;
            int v = m->visited && m->visited(node_attr(n, "href"), m->ctx);
            if (((ps & PS_LINK) && v) || ((ps & PS_VISITED) && !v))
                return 0;
        }
        if ((ps & PS_ROOT) && n->parent)
            return 0;
        if (ps & PS_EMPTY)
            for (const struct node *k = n->first; k; k = k->next)
                if (k->type == N_ELEMENT || k->len)
                    return 0;
        if (ps & PS_CHECKED) {
            int on = n->tag == T_INPUT && (!strcasecmp(input_kind(n), "checkbox") ||
                                           !strcasecmp(input_kind(n), "radio"))
                   ? checked_state(n) : node_attr(n, "checked") || node_attr(n, "selected");
            if (!on)
                return 0;
        }
        if (ps & PS_TARGET) {
            const char *id = node_attr(n, "id");
            if (!css_target_id || !*css_target_id || !id || strcmp(id, css_target_id))
                return 0;
        }
        if ((ps & PS_READ_WRITE) && !(read_write(n) || node_attr(n, "contenteditable")))
            return 0;
        if ((ps & PS_READ_ONLY) && (read_write(n) || node_attr(n, "contenteditable")))
            return 0;
        if (ps & PS_DEFAULT) {
            int on = (n->tag == T_INPUT && (!strcasecmp(input_kind(n), "checkbox") ||
                                            !strcasecmp(input_kind(n), "radio")) &&
                      node_attr(n, "checked")) ||
                     (n->tag == T_OPTION && node_attr(n, "selected")) || is_default_button(n);
            if (!on)
                return 0;
        }
        if (ps & PS_INDETERMINATE) {
            int on = (n->tag == T_INPUT && !strcasecmp(input_kind(n), "checkbox") && node_attr(n, "indeterminate")) ||
                     (n->tag == T_PROGRESS && !node_attr(n, "value"));
            if (!on)
                return 0;
        }
        if ((ps & PS_VALID) && !match_valid(n, 1))
            return 0;
        if ((ps & PS_INVALID) && !match_valid(n, 0))
            return 0;
        if (ps & (PS_IN_RANGE | PS_OUT_RANGE)) {
            long x, lo, hi;
            const char *t = n->tag == T_INPUT ? input_kind(n) : "";
            const char *mn = node_attr(n, "min"), *mx = node_attr(n, "max");
            if ((strcasecmp(t, "number") && strcasecmp(t, "range")) || (!mn && !mx))
                return 0;
            const char *v = field_value(n);
            int out = is_number(v, &x) && ((mn && is_number(mn, &lo) && x < lo) || (mx && is_number(mx, &hi) && x > hi));
            if (((ps & PS_IN_RANGE) && out) || ((ps & PS_OUT_RANGE) && !out))
                return 0;
        }
        if ((ps & PS_DISABLED) && !node_attr(n, "disabled"))
            return 0;
        if ((ps & PS_ENABLED) && node_attr(n, "disabled"))
            return 0;
    }
    if (c->lang) {
        const char *l = lang_of(n);
        size_t cl = strlen(c->lang);
        if (!l || strncasecmp(l, c->lang, cl) || (l[cl] && l[cl] != '-'))
            return 0;
    }
    if (c->has_nth) {
        if (!match_nth_filter(c, n, m))
            return 0;
        int idx = 1;
        if (c->nth_last) {
            for (const struct node *p = next_element(n); p; p = next_element(p))
                if ((!c->nth_of_type || same_type(p, n)) && match_nth_filter(c, p, m))
                    idx++;
        } else {
            for (const struct node *p = prev_element(n); p; p = prev_element(p))
                if ((!c->nth_of_type || same_type(p, n)) && match_nth_filter(c, p, m))
                    idx++;
        }
        if (!web_nth_fits(c->nth_a, c->nth_b, idx))
            return 0;
    }
    for (int i = 0; i < c->nnot; i++)
        if (match_from(&c->not_list[i], c->not_list[i].nparts - 1, n, m))
            return 0;
    if (c->nhas) {
        int any = 0;
        for (int i = 0; i < c->nhas && !any; i++) {
            for (const struct node *d = n->first; d && !any; d = d->next) {
                /* walk every descendant (or just the children) in document order */
                const struct node *k = d;
                while (k && !any) {
                    if (k->type == N_ELEMENT &&
                        match_from(&c->has_list[i], c->has_list[i].nparts - 1, k, m))
                        any = 1;
                    if (any || (c->has_child & 1u))
                        break;
                    if (k->first) {
                        k = k->first;
                    } else {
                        while (k != d && !k->next)
                            k = k->parent;
                        k = k == d ? NULL : k->next;
                    }
                }
            }
        }
        if (!any)
            return 0;
    }
    if (c->nis) {
        int any = 0;
        for (int i = 0; i < c->nis && !any; i++)
            any = match_from(&c->is_list[i], c->is_list[i].nparts - 1, n, m);
        if (!any)
            return 0;
    }
    return 1;
}

static int match_from(const struct selector *s, int i, const struct node *n, const struct mctx *m)
{
    if (!n) return 0;
    if (s->parts[i].nslot) {
        if (!m->host || n->parent != m->host || n->type != N_ELEMENT) return 0;
        struct mctx outer = *m;
        outer.host = NULL;
        if (!match_from(s->parts[i].slot_args, 0, n, &outer)) return 0;
        const char *name = node_attr(n, "slot");
        n = shadow_slot((struct node *)m->host, name ? name : "");
        if (!n) return 0;
    }
    if (!match_compound(&s->parts[i], n, m))
        return 0;
    if (i == 0)
        return 1;
    const struct node *p;
    switch (s->parts[i].comb) {
    case '>':
        p = n->parent;
        if (p && p->shadow_host == m->host && m->host) p = m->host;
        return p && match_from(s, i - 1, p, m);
    case '+':
        if (n == cur_subject)
            cur_flags |= SF_NOSHARE;    /* depends on where it stands among its siblings */
        p = prev_element(n);
        return p && match_from(s, i - 1, p, m);
    case '~':
        if (n == cur_subject)
            cur_flags |= SF_NOSHARE;
        for (p = prev_element(n); p; p = prev_element(p))
            if (match_from(s, i - 1, p, m))
                return 1;
        return 0;
    default:
        for (p = n->parent; p; p = p == m->host ? NULL : p->parent) {
            if (p->shadow_host == m->host && m->host) p = m->host;
            if (match_from(s, i - 1, p, m))
                return 1;
        }
        return 0;
    }
}

/* ---- values ---- */

struct vctx {                   /* what relative units are relative to */
    int font_size, root_font_size, vw, vh;
    struct arena *a;            /* for values kept in the style (grid tracks) */
    int lh, root_lh;            /* the line height (lh units) and the root's (rlh); 0: 1.2em */
    struct node *cq_from;       /* where cq units look for their container (NULL: the viewport) */
};

/* A style's line height in pixels, for lh units: normal counts as 1.2. */
static int line_px(const struct style *st)
{
    return st->lh_kind == LH_PX ? st->lh : st->lh_kind == LH_FACTOR ? st->font_size * st->lh / 100
         : st->font_size * 6 / 5;
}

static const struct named_color {
    const char *name;
    uint32_t rgb;
} named_colors[] = {
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff },
    { "aquamarine", 0x7fffd4 }, { "azure", 0xf0ffff }, { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 },
    { "black", 0x000000 }, { "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff },
    { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a }, { "burlywood", 0xdeb887 },
    { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 }, { "chocolate", 0xd2691e },
    { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc },
    { "crimson", 0xdc143c }, { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b },
    { "darkgoldenrod", 0xb8860b }, { "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 },
    { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b }, { "darkmagenta", 0x8b008b },
    { "darkolivegreen", 0x556b2f }, { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc },
    { "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f },
    { "darkslateblue", 0x483d8b }, { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f },
    { "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 }, { "deeppink", 0xff1493 },
    { "deepskyblue", 0x00bfff }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 },
    { "forestgreen", 0x228b22 }, { "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc },
    { "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 }, { "goldenrod", 0xdaa520 }, { "gray", 0x808080 },
    { "green", 0x008000 }, { "greenyellow", 0xadff2f }, { "grey", 0x808080 }, { "honeydew", 0xf0fff0 },
    { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c }, { "indigo", 0x4b0082 }, { "ivory", 0xfffff0 },
    { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa }, { "lavenderblush", 0xfff0f5 },
    { "lawngreen", 0x7cfc00 }, { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 },
    { "lightcoral", 0xf08080 }, { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 },
    { "lightgray", 0xd3d3d3 }, { "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 },
    { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a }, { "lightseagreen", 0x20b2aa },
    { "lightskyblue", 0x87cefa }, { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 },
    { "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 },
    { "limegreen", 0x32cd32 }, { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 },
    { "mediumaquamarine", 0x66cdaa }, { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 },
    { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 }, { "mediumslateblue", 0x7b68ee },
    { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc },
    { "mediumvioletred", 0xc71585 }, { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa },
    { "mistyrose", 0xffe4e1 }, { "moccasin", 0xffe4b5 }, { "navajowhite", 0xffdead },
    { "navy", 0x000080 }, { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 }, { "olivedrab", 0x6b8e23 },
    { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 },
    { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee },
    { "palevioletred", 0xdb7093 }, { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 },
    { "peru", 0xcd853f }, { "pink", 0xffc0cb }, { "plum", 0xdda0dd }, { "powderblue", 0xb0e0e6 },
    { "purple", 0x800080 }, { "rebeccapurple", 0x663399 }, { "red", 0xff0000 },
    { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 }, { "saddlebrown", 0x8b4513 },
    { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 },
    { "seashell", 0xfff5ee }, { "sienna", 0xa0522d }, { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb },
    { "slateblue", 0x6a5acd }, { "slategray", 0x708090 }, { "slategrey", 0x708090 },
    { "snow", 0xfffafa }, { "springgreen", 0x00ff7f }, { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c },
    { "teal", 0x008080 }, { "thistle", 0xd8bfd8 }, { "tomato", 0xff6347 }, { "turquoise", 0x40e0d0 },
    { "violet", 0xee82ee }, { "wheat", 0xf5deb3 }, { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 },
    { "yellow", 0xffff00 }, { "yellowgreen", 0x9acd32 },
};

/* A number as fixed point times 100; returns the characters used, 0 if none. */
static int parse_num(const char *s, int *out)
{
    const char *p = s;
    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    if (!isdigit((unsigned char)*p) && !(*p == '.' && isdigit((unsigned char)p[1])))
        return 0;
    long v = 0;
    while (isdigit((unsigned char)*p)) {
        if (v < 100000000)
            v = v * 10 + (*p - '0');
        p++;
    }
    v *= 100;
    if (*p == '.') {
        p++;
        int scale = 10;
        while (isdigit((unsigned char)*p)) {
            v += (*p - '0') * scale;
            scale /= 10;
            p++;
        }
    }
    if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) || p[1] == '-')) {
        int e = 0, eneg = 0;
        p++;
        if (*p == '-')
            eneg = 1, p++;
        while (isdigit((unsigned char)*p))
            e = e * 10 + (*p++ - '0');
        while (e-- > 0)
            v = eneg ? v / 10 : v * 10;
    }
    *out = (int)(neg ? -v : v);
    return (int)(p - s);
}

static int hexval(char c)
{
    return isdigit((unsigned char)c) ? c - '0' : tolower((unsigned char)c) - 'a' + 10;
}

static int clamp255(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Hue to RGB for hsl(), all in fixed point (times 100, hue in degrees). */
static int hue_rgb(int p, int q, int t)
{
    t = ((t % 36000) + 36000) % 36000;
    if (t < 6000)
        return p + (q - p) * t / 6000;
    if (t < 18000)
        return q;
    if (t < 24000)
        return p + (q - p) * (24000 - t) / 6000;
    return p;
}

int css_parse_color(const char *s, int n, uint32_t *out)
{
    char buf[320];
    while (n > 0 && isspace((unsigned char)*s))
        s++, n--;
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    if (n <= 0 || n >= (int)sizeof(buf))
        return -1;
    for (int i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)s[i]);
    buf[n] = 0;
    if (buf[0] == '#') {
        for (int i = 1; i < n; i++)
            if (!isxdigit((unsigned char)buf[i]))
                return -1;
        int r, g, b, al = 255;
        if (n == 4 || n == 5) {
            r = hexval(buf[1]) * 17, g = hexval(buf[2]) * 17, b = hexval(buf[3]) * 17;
            if (n == 5)
                al = hexval(buf[4]) * 17;
        } else if (n == 7 || n == 9) {
            r = hexval(buf[1]) * 16 + hexval(buf[2]);
            g = hexval(buf[3]) * 16 + hexval(buf[4]);
            b = hexval(buf[5]) * 16 + hexval(buf[6]);
            if (n == 9)
                al = hexval(buf[7]) * 16 + hexval(buf[8]);
        } else {
            return -1;
        }
        *out = (uint32_t)al << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        return 0;
    }
    if (!strcmp(buf, "transparent")) {
        *out = TRANSPARENT;
        return 0;
    }
    if (!strncmp(buf, "rgb", 3) || !strncmp(buf, "hsl", 3)) {
        char *p = strchr(buf, '(');
        if (!p)
            return -1;
        p++;
        while (*p == ' ')
            p++;
        if (!strncmp(p, "from ", 5))
            return css_color_function(buf, out);   /* a relative color (csscolor.c) */
        int v[4] = { 0, 0, 0, 100 }, pct[4] = { 0 }, k = 0;
        while (*p && *p != ')' && k < 4) {
            while (*p == ' ' || *p == ',' || *p == '/')
                p++;
            int used = parse_num(p, &v[k]);
            if (!used)
                return -1;
            p += used;
            if (*p == '%')
                pct[k] = 1, p++;
            else if (!strncmp(p, "deg", 3))
                p += 3;
            k++;
            while (*p == ' ')
                p++;
        }
        if (k < 3)
            return -1;
        int al = pct[3] ? v[3] * 255 / 10000 : v[3] * 255 / 100;
        int r, g, b;
        if (buf[0] == 'r') {
            r = pct[0] ? v[0] * 255 / 10000 : v[0] / 100;
            g = pct[1] ? v[1] * 255 / 10000 : v[1] / 100;
            b = pct[2] ? v[2] * 255 / 10000 : v[2] / 100;
        } else {
            int h = v[0], sat = v[1], l = v[2];      /* degrees*100, percent*100 */
            int q = l < 5000 ? l * (10000 + sat) / 10000 : l + sat - l * sat / 10000;
            int pp = 2 * l - q;
            r = hue_rgb(pp, q, h + 12000) * 255 / 10000;
            g = hue_rgb(pp, q, h) * 255 / 10000;
            b = hue_rgb(pp, q, h - 12000) * 255 / 10000;
        }
        *out = (uint32_t)clamp255(al) << 24 | (uint32_t)clamp255(r) << 16 |
               (uint32_t)clamp255(g) << 8 | (uint32_t)clamp255(b);
        return 0;
    }
    if (!strncmp(buf, "oklab(", 6) || !strncmp(buf, "oklch(", 6) || !strncmp(buf, "lab(", 4) ||
        !strncmp(buf, "lch(", 4) || !strncmp(buf, "hwb(", 4) || !strncmp(buf, "color(", 6) ||
        !strncmp(buf, "color-mix(", 10))
        return css_color_function(buf, out);       /* csscolor.c */
    if (!strncmp(buf, "light-dark(", 11)) {         /* we are always in light mode */
        const char *p = buf + 11;
        const char *comma = find_top(p, buf + n, ',');
        return css_parse_color(s + (p - buf), (int)(comma - p), out);
    }
    int lo = 0, hi = (int)(sizeof(named_colors) / sizeof(named_colors[0])) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(named_colors[mid].name, buf);
        if (!c) {
            *out = OPAQUE(named_colors[mid].rgb);
            return 0;
        }
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    /* System colors (CSS Color 4's, and the older Windows ones some pages use), in light mode. */
    static const struct named_color system_colors[] = {
        { "accentcolor", 0x0a64d6 }, { "accentcolortext", 0xffffff }, { "activeborder", 0xc0c0c0 },
        { "activecaption", 0x000080 }, { "activetext", 0xee0000 }, { "appworkspace", 0x808080 },
        { "background", 0x008080 }, { "buttonborder", 0x767676 }, { "buttonface", 0xc0c0c0 },
        { "buttonhighlight", 0xffffff }, { "buttonshadow", 0x808080 }, { "buttontext", 0x000000 },
        { "canvas", 0xffffff }, { "canvastext", 0x000000 }, { "captiontext", 0xffffff }, { "field", 0xffffff },
        { "fieldtext", 0x000000 }, { "graytext", 0x6d6d6d }, { "highlight", 0x0a64d6 },
        { "highlighttext", 0xffffff }, { "inactiveborder", 0xc0c0c0 }, { "inactivecaption", 0x808080 },
        { "inactivecaptiontext", 0xc0c0c0 }, { "infobackground", 0xffffe1 }, { "infotext", 0x000000 },
        { "linktext", 0x0000ee }, { "mark", 0xffff00 }, { "marktext", 0x000000 }, { "menu", 0xc0c0c0 },
        { "menutext", 0x000000 }, { "scrollbar", 0xc0c0c0 }, { "selecteditem", 0x0a64d6 },
        { "selecteditemtext", 0xffffff }, { "threeddarkshadow", 0x000000 }, { "threedface", 0xc0c0c0 },
        { "threedhighlight", 0xffffff }, { "threedlightshadow", 0xdfdfdf }, { "threedshadow", 0x808080 },
        { "visitedtext", 0x551a8b }, { "window", 0xffffff }, { "windowframe", 0x000000 },
        { "windowtext", 0x000000 },
    };
    for (size_t i = 0; i < sizeof(system_colors) / sizeof(system_colors[0]); i++)
        if (!strcmp(buf, system_colors[i].name)) {
            *out = OPAQUE(system_colors[i].rgb);
            return 0;
        }
    return -1;
}

/* num (times 100) of `unit` in hundredths of a pixel; 0 for an unknown unit. */
static int unit_px100(const char *unit, long num, const struct vctx *v, long *out)
{
    long px;
    int absolute = 1;
    if (!strcmp(unit, "px"))
        px = num;
    else if (!strcmp(unit, "em"))
        px = num * v->font_size, absolute = 0;      /* the font size is zoomed already */
    else if (!strcmp(unit, "rem"))
        px = num * v->root_font_size, absolute = 0;
    else if (!strcmp(unit, "lh"))
        px = num * (v->lh ? v->lh : v->font_size * 6 / 5), absolute = 0;
    else if (!strcmp(unit, "rlh"))
        px = num * (v->root_lh ? v->root_lh : v->root_font_size * 6 / 5), absolute = 0;
    else if (!strcmp(unit, "ic"))
        px = num * v->font_size, absolute = 0;         /* a CJK ideograph: about an em */
    else if (!strcmp(unit, "cap"))
        px = num * v->font_size * 7 / 10, absolute = 0;
    else if (!strcmp(unit, "ex") || !strcmp(unit, "ch"))
        px = num * v->font_size / 2, absolute = 0;
    else if (!strcmp(unit, "pt"))
        px = num * 4 / 3;
    else if (!strcmp(unit, "pc"))
        px = num * 16;
    else if (!strcmp(unit, "in"))
        px = num * 96;
    else if (!strcmp(unit, "cm"))
        px = num * 96 * 100 / 254;
    else if (!strcmp(unit, "mm"))
        px = num * 96 * 10 / 254;
    else if (unit[0] == 'c' && unit[1] == 'q') {        /* the nearest container's box, else the viewport */
        int cw = v->vw, ch = v->vh;
        struct node *c = v->cq_from ? cq_container(v->cq_from, NULL) : NULL;
        if (c) {
            int w, h;
            cq_size(c, &w, &h);
            cw = w;
            if (c->style->cq_type == CQ_SIZE)
                ch = h;
        }
        if (!strcmp(unit, "cqw") || !strcmp(unit, "cqi"))
            px = num * cw / 100;
        else if (!strcmp(unit, "cqh") || !strcmp(unit, "cqb"))
            px = num * ch / 100;
        else if (!strcmp(unit, "cqmin"))
            px = num * (cw < ch ? cw : ch) / 100;
        else if (!strcmp(unit, "cqmax"))
            px = num * (cw > ch ? cw : ch) / 100;
        else
            return 0;
        absolute = 0;
    } else if (!strcmp(unit, "vw") || !strcmp(unit, "dvw") || !strcmp(unit, "svw") || !strcmp(unit, "lvw") ||
             !strcmp(unit, "vi"))
        px = num * v->vw / 100, absolute = 0;
    else if (!strcmp(unit, "vh") || !strcmp(unit, "dvh") || !strcmp(unit, "svh") || !strcmp(unit, "lvh") ||
             !strcmp(unit, "vb"))
        px = num * v->vh / 100, absolute = 0;
    else if (!strcmp(unit, "vmin") || !strcmp(unit, "dvmin") || !strcmp(unit, "svmin") || !strcmp(unit, "lvmin"))
        px = num * (v->vw < v->vh ? v->vw : v->vh) / 100, absolute = 0;
    else if (!strcmp(unit, "vmax") || !strcmp(unit, "dvmax") || !strcmp(unit, "svmax") || !strcmp(unit, "lvmax"))
        px = num * (v->vw > v->vh ? v->vw : v->vh) / 100, absolute = 0;
    else
        return 0;
    *out = absolute && css_zoom != 100 ? px * css_zoom / 100 : px;
    return 1;
}

/* ---- calc(), min(), max() and clamp() ---- */

struct cval {
    long px, pct;               /* hundredths of a pixel, and percent times 100 */
    int num;                    /* a plain number (times 100), held in px */
    int ok;
    int pick;                   /* 1 the smaller, 2 the larger of px and pct (not a sum) */
    int angle;                  /* a number that is an angle, in degrees (times 100) */
};

/* A value to do arithmetic on: a pick of px and percent becomes the percent. */
static struct cval calc_flat(struct cval a)
{
    if (a.pick) {
        a.px = 0;
        a.pick = 0;
    }
    return a;
}

static struct cval calc_sum(const char **p, const struct vctx *v);

static void calc_ws(const char **p)
{
    while (isspace((unsigned char)**p))
        (*p)++;
}

/* Of two values, the smaller (or larger). Percentages are unknown until
 * layout, so a pixel size and a percentage are kept for it to choose
 * between (min(100%, 600px)); other mixes keep the one with a percentage. */
static struct cval calc_pick(struct cval a, struct cval b, int larger)
{
    if (!a.ok || !b.ok)
        return a.ok ? a : b;
    a = calc_flat(a);
    b = calc_flat(b);
    if (!a.pct && !b.pct)
        return (larger ? a.px >= b.px : a.px <= b.px) ? a : b;
    if (!a.px && !b.px)
        return (larger ? a.pct >= b.pct : a.pct <= b.pct) ? a : b;
    if ((!a.pct && !b.px) || (!a.px && !b.pct)) {
        struct cval r = { a.px + b.px, a.pct + b.pct, 0, 1, larger ? 2 : 1, 0 };
        return r;
    }
    return a.pct ? a : b;
}

/* min(a, b, ...) (kind 0), max(...) (1) or clamp(lo, val, hi) (2), *p past the '('. */
static struct cval calc_func(const char **p, const struct vctx *v, int kind)
{
    struct cval args[8], r = { 0, 0, 0, 0, 0, 0 };
    int n = 0;
    for (;;) {
        struct cval a = calc_sum(p, v);
        if (n < 8)
            args[n++] = a;
        calc_ws(p);
        if (**p != ',')
            break;
        (*p)++;
    }
    if (**p != ')')
        return r;
    (*p)++;
    if (kind == 2)
        return n == 3 ? calc_pick(args[0], calc_pick(args[1], args[2], 0), 1) : r;
    r = args[0];
    for (int i = 1; i < n; i++)
        r = calc_pick(r, args[i], kind == 1);
    return r;
}

/* The comma-separated arguments of a math function, *p past its '('; returns how many (-1 if broken). */
static int calc_args(const char **p, const struct vctx *v, struct cval *args, int max)
{
    int n = 0;
    for (;;) {
        struct cval a = calc_flat(calc_sum(p, v));
        if (n < max)
            args[n++] = a;
        calc_ws(p);
        if (**p != ',')
            break;
        (*p)++;
    }
    if (**p != ')')
        return -1;
    (*p)++;
    return n;
}

static struct cval calc_number(double x, int angle)
{
    struct cval r = { 0, 0, 1, 1, 0, angle };
    if (x != x)
        x = 0;                          /* NaN */
    if (x > 1e7)
        x = 1e7;
    if (x < -1e7)
        x = -1e7;
    r.px = (long)(x * 100 + (x < 0 ? -0.5 : 0.5));
    return r;
}

/* A value's magnitude to compare or divide with another's of the same kind: its pixels, its
 * percentage, or its number. -1 in *kind when it is a mix (10px + 5%), which cannot be. */
static double calc_scalar(struct cval a, int *kind)
{
    if (a.pct && a.px && !a.num) {
        *kind = -1;
        return 0;
    }
    *kind = a.num ? 0 : a.pct ? 2 : 1;
    return a.pct && !a.num ? a.pct / 100.0 : a.px / 100.0;
}

static struct cval calc_like(struct cval like, double x)
{
    struct cval r = like;
    long v = (long)(x * 100 + (x < 0 ? -0.5 : 0.5));
    r.pick = 0;
    if (like.pct && !like.num)
        r.pct = v, r.px = 0;
    else
        r.px = v, r.pct = 0;
    return r;
}

/* round(), mod(), rem(), abs(), sign(), the trigonometric functions, pow(), sqrt(), hypot(), log() and exp(). */
static struct cval calc_math(const char **p, const struct vctx *v, const char *fn)
{
    struct cval bad = { 0, 0, 0, 0, 0, 0 }, a[8];
    int strategy = 0;                   /* round(): 0 nearest, 1 up, 2 down, 3 to-zero */
    if (!strcmp(fn, "round")) {
        static const char *const names[] = { "nearest", "up", "down", "to-zero" };
        calc_ws(p);
        for (int k = 0; k < 4; k++) {
            size_t l = strlen(names[k]);
            if (!strncasecmp(*p, names[k], l) && ((*p)[l] == ',' || isspace((unsigned char)(*p)[l]))) {
                strategy = k;
                *p += l;
                calc_ws(p);
                if (**p == ',')
                    (*p)++;
                break;
            }
        }
    }
    int n = calc_args(p, v, a, 8);
    if (n < 1)
        return bad;
    for (int i = 0; i < n; i++)
        if (!a[i].ok)
            return bad;
    int k0, k1;
    double x = calc_scalar(a[0], &k0);
    if (k0 < 0)
        return bad;
    if (!strcmp(fn, "abs"))
        return calc_like(a[0], fabs(x));
    if (!strcmp(fn, "sign"))
        return calc_number(x > 0 ? 1 : x < 0 ? -1 : 0, 0);
    if (!strcmp(fn, "round") || !strcmp(fn, "mod") || !strcmp(fn, "rem")) {
        double y = 1;
        if (n >= 2) {
            y = calc_scalar(a[1], &k1);
            if (k1 < 0 || (k1 != k0 && k1 && k0))
                return bad;
        } else if (strcmp(fn, "round")) {
            return bad;
        }
        if (y == 0)
            return bad;
        double q = x / y, r;
        if (!strcmp(fn, "round")) {
            double f = floor(q);
            r = strategy == 1 ? ceil(q) : strategy == 2 ? f : strategy == 3 ? (q < 0 ? ceil(q) : f)
              : (q - f >= 0.5 ? f + 1 : f);
            return calc_like(a[0], r * y);
        }
        r = fmod(x, y);                 /* rem: the sign of x */
        if (!strcmp(fn, "mod") && r != 0 && (r < 0) != (y < 0))
            r += y;                     /* mod: the sign of y */
        return calc_like(a[0], r);
    }
    if (!strcmp(fn, "hypot")) {
        double sum = 0;
        for (int i = 0; i < n; i++) {
            double t = calc_scalar(a[i], &k1);
            if (k1 < 0 || (k1 != k0 && k1 && k0))
                return bad;
            sum += t * t;
        }
        return calc_like(a[0], sqrt(sum));
    }
    /* the rest take numbers (and angles) */
    double rad = a[0].angle ? x * M_PI / 180 : x;
    if (!strcmp(fn, "sin")) return calc_number(sin(rad), 0);
    if (!strcmp(fn, "cos")) return calc_number(cos(rad), 0);
    if (!strcmp(fn, "tan")) return calc_number(tan(rad), 0);
    if (!strcmp(fn, "asin")) return calc_number(asin(x) * 180 / M_PI, 1);
    if (!strcmp(fn, "acos")) return calc_number(acos(x) * 180 / M_PI, 1);
    if (!strcmp(fn, "atan")) return calc_number(atan(x) * 180 / M_PI, 1);
    if (!strcmp(fn, "atan2") && n == 2) {
        double y = calc_scalar(a[1], &k1);
        return k1 == k0 ? calc_number(atan2(x, y) * 180 / M_PI, 1) : bad;
    }
    if (!strcmp(fn, "pow") && n == 2) return calc_number(pow(x, a[1].px / 100.0), 0);
    if (!strcmp(fn, "sqrt")) return x < 0 ? bad : calc_number(sqrt(x), 0);
    if (!strcmp(fn, "exp")) return calc_number(exp(x), 0);
    if (!strcmp(fn, "log")) {
        if (x <= 0)
            return bad;
        double base = n == 2 ? a[1].px / 100.0 : 0;
        return calc_number(base > 0 && base != 1 ? log(x) / log(base) : log(x), 0);
    }
    return bad;
}

/* Is s a CSS math function's name and '(' (calc(), round()...)? Returns the name's length. */
static int calc_function_at(const char *s)
{
    static const char *const names[] = {
        "calc", "-webkit-calc", "min", "max", "clamp", "round", "mod", "rem", "abs", "sign", "sin", "cos", "tan",
        "asin", "acos", "atan", "atan2", "pow", "sqrt", "hypot", "log", "exp",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        size_t l = strlen(names[i]);
        if (!strncasecmp(s, names[i], l) && s[l] == '(')
            return (int)l;
    }
    return 0;
}

static struct cval calc_term(const char **p, const struct vctx *v)
{
    static const struct { const char *name; int kind; } funcs[] = {
        { "calc(", -1 }, { "-webkit-calc(", -1 }, { "min(", 0 }, { "max(", 1 }, { "clamp(", 2 },
    };
    struct cval r = { 0, 0, 0, 0, 0, 0 };
    calc_ws(p);
    for (size_t i = 0; i < sizeof(funcs) / sizeof(funcs[0]); i++) {
        size_t fl = strlen(funcs[i].name);
        if (strncasecmp(*p, funcs[i].name, fl))
            continue;
        *p += fl;
        if (funcs[i].kind >= 0)
            return calc_func(p, v, funcs[i].kind);
        (*p)--;                         /* calc( is a bracket */
        break;
    }
    int fl = **p != '(' ? calc_function_at(*p) : 0;
    if (fl) {
        char fn[16];
        snprintf(fn, sizeof(fn), "%.*s", fl, *p);
        for (char *q = fn; *q; q++)
            *q = (char)tolower((unsigned char)*q);
        *p += fl + 1;
        return calc_math(p, v, fn);
    }
    if (**p == '(') {
        (*p)++;
        r = calc_sum(p, v);
        calc_ws(p);
        if (**p != ')')
            r.ok = 0;
        else
            (*p)++;
        return r;
    }
    /* the constants */
    if (isalpha((unsigned char)**p) || (**p == '-' && isalpha((unsigned char)(*p)[1]))) {
        static const struct { const char *name; double v; } consts[] = {
            { "pi", M_PI }, { "e", M_E }, { "infinity", 1e7 }, { "-infinity", -1e7 }, { "nan", 0 },
        };
        for (size_t i = 0; i < sizeof(consts) / sizeof(consts[0]); i++) {
            size_t l = strlen(consts[i].name);
            if (!strncasecmp(*p, consts[i].name, l) && !isalnum((unsigned char)(*p)[l]) && (*p)[l] != '-') {
                *p += l;
                return calc_number(consts[i].v, 0);
            }
        }
        return r;
    }
    int num, used = parse_num(*p, &num);
    if (!used)
        return r;
    const char *u = *p + used;
    char unit[8];
    int ul = 0;
    while (ul < 7 && (isalpha((unsigned char)u[ul]) || u[ul] == '%'))
        unit[ul] = (char)tolower((unsigned char)u[ul]), ul++;
    unit[ul] = 0;
    *p = u + ul;
    r.ok = 1;
    if (!unit[0])
        r.num = 1, r.px = num;
    else if (!strcmp(unit, "%"))
        r.pct = num;
    else if (!strcmp(unit, "deg") || !strcmp(unit, "rad") || !strcmp(unit, "grad") || !strcmp(unit, "turn"))
        r = calc_number(num / 100.0 * (unit[0] == 'd' ? 1 : unit[0] == 'r' ? 180 / M_PI : unit[0] == 'g' ? 0.9 : 360), 1);
    else if (!unit_px100(unit, num, v, &r.px))
        r.ok = 0;
    return r;
}

static struct cval calc_product(const char **p, const struct vctx *v)
{
    struct cval a = calc_term(p, v);
    for (;;) {
        calc_ws(p);
        char op = **p;
        if (op != '*' && op != '/')
            return a;
        (*p)++;
        struct cval b = calc_flat(calc_term(p, v));
        a = calc_flat(a);
        if (!a.ok || !b.ok)
            return (struct cval){ 0, 0, 0, 0, 0, 0 };
        if (op == '*' && (b.num || a.num)) {
            struct cval k = b.num ? b : a, x = b.num ? a : b;
            x.px = x.px * k.px / 100;
            x.pct = x.pct * k.px / 100;
            x.num = a.num && b.num;
            a = x;
        } else if (op == '/' && b.num && b.px) {
            a.px = a.px * 100 / b.px;
            a.pct = a.pct * 100 / b.px;
        } else {
            a.ok = 0;
        }
    }
}

static struct cval calc_sum(const char **p, const struct vctx *v)
{
    struct cval a = calc_product(p, v);
    for (;;) {
        calc_ws(p);
        char op = **p;
        /* + and - between terms have spaces round them; "-5px" is a number */
        if ((op != '+' && op != '-') || !isspace((unsigned char)(*p)[1]))
            return a;
        (*p)++;
        struct cval b = calc_flat(calc_product(p, v));
        a = calc_flat(a);
        if (!a.ok || !b.ok)
            return (struct cval){ 0, 0, 0, 0, 0, 0 };
        long sign = op == '-' ? -1 : 1;
        a.px += sign * b.px;
        a.pct += sign * b.pct;
        a.num = a.num && b.num;
    }
}

/*
 * A length in pixels. base is the font size for em; unitless numbers are
 * pixels (as quirks mode allows). Returns 0 for anything unparseable.
 * calc() and friends are worked out, except that percentages stay
 * percentages until layout knows what they are of (U_CALC when mixed).
 */
static int length_value(const char *s, const struct vctx *v, struct len *out)
{
    while (isspace((unsigned char)*s))
        s++;
    out->pct = 0;
    if (!strncasecmp(s, "auto", 4)) {
        out->unit = U_AUTO;
        out->v = 0;
        return 1;
    }
    if (calc_function_at(s)) {
        const char *p = s;
        struct cval r = calc_term(&p, v);
        if (!r.ok || (r.num && r.px && !r.angle))
            return r.ok && r.num ? (out->unit = U_PX, out->v = (int)(r.px * css_zoom / 10000), 1) : 0;
        if (r.angle)
            return 0;
        long px = (r.px + (r.px >= 0 ? 50 : -50)) / 100;
        if (r.pick) {
            out->unit = r.pick == 1 ? U_MIN : U_MAX;
            out->v = (int)px;
            out->pct = (int)r.pct;
        } else if (!r.pct) {
            out->unit = U_PX;
            out->v = (int)px;
        } else if (!px) {
            out->unit = U_PCT;
            out->v = (int)r.pct;
        } else {
            out->unit = U_CALC;
            out->v = (int)px;
            out->pct = (int)r.pct;
        }
        return 1;
    }
    int num;
    int used = parse_num(s, &num);
    if (!used)
        return 0;
    const char *u = s + used;
    char unit[8];
    int ul = 0;
    while (ul < 7 && (isalpha((unsigned char)u[ul]) || u[ul] == '%'))
        unit[ul] = (char)tolower((unsigned char)u[ul]), ul++;
    unit[ul] = 0;
    long px;
    out->unit = U_PX;
    if (!strcmp(unit, "%")) {
        out->unit = U_PCT;
        out->v = num;
        return 1;
    }
    if (!unit[0])
        px = num * css_zoom / 100;
    else if (!unit_px100(unit, num, v, &px))
        return 0;
    out->v = (int)((px + (px >= 0 ? 50 : -50)) / 100);
    return 1;
}

/* A length on a base: percentages (and calc()'s) are of it. */
static int len_on(struct len l, int base)
{
    if (l.unit == U_PCT)
        return base * l.v / 10000;
    if (l.unit == U_CALC)
        return l.v + base * l.pct / 10000;
    if (l.unit == U_MIN || l.unit == U_MAX) {
        int p = base * l.pct / 10000;
        return (l.unit == U_MIN) == (p < l.v) ? p : l.v;
    }
    return l.v;
}

/* Something length_value can read (for @supports). */
static int is_length(const char *s)
{
    struct vctx v = { ZOOM(16), ZOOM(16), 1024, 768, NULL, 0, 0, NULL };
    struct len l;
    return length_value(s, &v, &l);
}

static int perspective_supported(const char *value)
{
    struct vctx v={16,16,1024,768,NULL,0,0,NULL};struct len l;
    return !strcasecmp(value,"none") || (length_value(value,&v,&l) && l.unit==U_PX && l.v>=0);
}

static int length_px(const char *s, int base)
{
    struct vctx v = { base, 16, 1024, 768, NULL, 0, 0, NULL };
    struct len l;
    if (!length_value(s, &v, &l) || l.unit != U_PX)
        return 0;
    return l.v;
}

/* Split a value into its top-level space-separated words. */
static int words(const char *s, const char **w, int *wl, int max)
{
    int n = 0;
    const char *end = s + strlen(s);
    while (*s && n < max) {
        while (isspace((unsigned char)*s) || *s == ',')
            s++;
        if (!*s)
            break;
        const char *e = s;
        while (e < end && !isspace((unsigned char)*e) && *e != ',') {
            if (*e == '(' || *e == '[' || *e == '"' || *e == '\'')
                e = skip_group(e, end);
            else
                e++;
        }
        w[n] = s;
        wl[n] = (int)(e - s);
        n++;
        s = e;
    }
    return n;
}

static int word_is(const char *w, int wl, const char *kw)
{
    return (int)strlen(kw) == wl && !strncasecmp(w, kw, (size_t)wl);
}

/* ---- applying declarations ---- */

static struct style initial_style;

static void init_initial(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    struct style *s = &initial_style;
    memset(s, 0, sizeof(*s));
    s->display = D_INLINE;
    s->family = FAM_SERIF;
    s->font_size = 16;
    s->color = OPAQUE(0);
    s->bg = TRANSPARENT;
    for (int i = 0; i < 4; i++) {
        s->margin[i].unit = U_PX;
        s->padding[i].unit = U_PX;
        s->border_w[i] = 3;
        s->border_style[i] = BS_NONE;
        s->border_color[i] = 1;         /* currentColor, resolved at the end */
    }
    s->width.unit = s->height.unit = s->max_width.unit = s->min_width.unit = U_AUTO;
    s->min_height.unit = U_PX;
    s->max_height.unit = U_AUTO;
    s->top.unit = s->right.unit = s->bottom.unit = s->left.unit = U_AUTO;
    s->flex_basis.unit = U_AUTO;
    s->deco_thickness.unit = s->underline_offset.unit = U_AUTO;
    s->flex_shrink = 100;
    s->align_items = s->justify = s->justify_items = AL_NORMAL;
    s->lh_kind = LH_NORMAL;
    s->list_style = LS_DISC;
    s->outline_color = 1;
    s->outline_w = 3;
    s->tab_size = 8;
    s->bg_w.unit = s->bg_h.unit = U_AUTO;
    s->obj_px = s->obj_py = (struct len){ 5000, 0, U_PCT };
    s->tox = s->toy = s->pox = s->poy = (struct len){ 5000, 0, U_PCT };
    s->perspective.unit = U_AUTO;
    s->row_gap_normal = 1;
    s->opacity = 255;
    s->col_gap_normal = 1;
    s->col_width.unit = U_AUTO;
    s->col_rule_w = 3;
    s->col_rule_color = 1;
}

static void inherit(struct style *st, const struct style *p)
{
    *st = initial_style;
    st->family = p->family;
    st->bold = p->bold;
    st->italic = p->italic;
    st->text_align = p->text_align;
    st->center_blocks = p->center_blocks;
    st->white_space = p->white_space;
    st->text_transform = p->text_transform;
    st->decoration = p->decoration;     /* not inherited in CSS, but drawn through children */
    st->list_style = p->list_style;
    st->hidden = p->hidden;
    st->lh_kind = p->lh_kind;
    st->lh = p->lh;
    st->font_size = p->font_size;
    st->color = p->color;
    st->link_color = p->link_color;
    st->border_spacing = p->border_spacing;
    st->vars = p->vars;
    st->text_indent = p->text_indent;
    st->tab_size = p->tab_size;
    st->ls_url = p->ls_url;
    st->letter_spacing = p->letter_spacing;
    st->word_spacing = p->word_spacing;
    st->word_break = p->word_break;
    st->small_caps = p->small_caps;
    st->list_inside = p->list_inside;
    st->empty_hide = p->empty_hide;
    st->deco_style = p->deco_style;
    st->deco_thickness = p->deco_thickness;
    st->underline_offset = p->underline_offset;
    st->deco_color = p->deco_color;
    st->ts_x = p->ts_x, st->ts_y = p->ts_y, st->ts_blur = p->ts_blur;
    st->ts_color = p->ts_color;
    st->cursor = p->cursor;
    st->pointer_none = p->pointer_none;
    st->select_none = p->select_none;
}

static int parse_border_style(const char *w, int wl)
{
    static const char *const names[] = { "none", "solid", "dotted", "dashed", "double", "inset",
                                         "outset", "groove", "ridge", "hidden" };
    for (int i = 0; i < 10; i++)
        if (word_is(w, wl, names[i]))
            return i == 9 ? BS_NONE : i;
    return -1;
}

static int border_width_kw(const char *w, int wl, const struct vctx *v, int *px)
{
    if (word_is(w, wl, "thin"))
        return *px = ZOOM(1) ? ZOOM(1) : 1, 1;
    if (word_is(w, wl, "medium"))
        return *px = ZOOM(3), 1;
    if (word_is(w, wl, "thick"))
        return *px = ZOOM(5), 1;
    char tmp[32];
    if (wl >= (int)sizeof(tmp))
        return 0;
    memcpy(tmp, w, (size_t)wl);
    tmp[wl] = 0;
    struct len l;
    if (length_value(tmp, v, &l) && l.unit == U_PX && (isdigit((unsigned char)w[0]) || w[0] == '.'))
        return *px = l.v < 0 ? 0 : l.v, 1;
    return 0;
}

/* "1px solid red" for the sides in mask (bit 0 top, 1 right, 2 bottom, 3 left). */
static void apply_border(struct style *st, const char *value, int mask, const struct vctx *v)
{
    const char *w[8];
    int wl[8], n = words(value, w, wl, 8);
    int width = ZOOM(3), style = BS_NONE;
    uint32_t color = 1;
    for (int i = 0; i < n; i++) {
        int bs = parse_border_style(w[i], wl[i]), px;
        uint32_t c;
        if (bs >= 0)
            style = bs;
        else if (border_width_kw(w[i], wl[i], v, &px))
            width = px;
        else if (css_parse_color(w[i], wl[i], &c) == 0)
            color = c;
    }
    for (int side = 0; side < 4; side++) {
        if (!(mask & 1 << side))
            continue;
        st->border_w[side] = width;
        st->border_style[side] = (uint8_t)style;
        st->border_color[side] = color;
    }
}

/* margin: 1px 2px -> top/bottom 1, left/right 2; returns the values per side. */
static int box_values(const char *value, const struct vctx *v, struct len out[4])
{
    const char *w[4];
    int wl[4], n = words(value, w, wl, 4);
    struct len l[4];
    for (int i = 0; i < n; i++) {
        char tmp[64];
        if (wl[i] >= (int)sizeof(tmp))
            return 0;
        memcpy(tmp, w[i], (size_t)wl[i]);
        tmp[wl[i]] = 0;
        if (!length_value(tmp, v, &l[i]))
            return 0;
    }
    switch (n) {
    case 1: out[0] = out[1] = out[2] = out[3] = l[0]; break;
    case 2: out[0] = out[2] = l[0]; out[1] = out[3] = l[1]; break;
    case 3: out[0] = l[0]; out[1] = out[3] = l[1]; out[2] = l[2]; break;
    case 4: out[0] = l[0]; out[1] = l[1]; out[2] = l[2]; out[3] = l[3]; break;
    default: return 0;
    }
    return 1;
}

int (*css_font_lookup)(void *,const char *);
void *css_font_context;

static int family_of(const char *value, int fallback)
{
    int n=0;
    const char *p=value;
    while(*p && n++<16){
        while(isspace((unsigned char)*p)||*p==',')p++;
        char f[128];int fl=0,quote=0;
        while(*p && (quote||*p!=',')){
            if((*p=='"'||*p=='\'')&&(!quote||quote==*p)){quote=quote?0:*p;p++;continue;}
            if(fl<127)f[fl++]=(char)tolower((unsigned char)*p);
            p++;
        }
        while(fl&&isspace((unsigned char)f[fl-1]))fl--;
        f[fl]=0;
        if(css_font_lookup){int family=css_font_lookup(css_font_context,f);if(family>=3)return family;}
        if (strstr(f, "mono") || strstr(f, "courier") || strstr(f, "consol") || strstr(f, "menlo") ||
            strstr(f, "monaco") || !strcmp(f, "fixed") || strstr(f, "code") || strstr(f, "terminal"))
            return FAM_MONO;
        if (!strcmp(f, "serif") || strstr(f, "times") || strstr(f, "georgia") ||
            strstr(f, "garamond") || strstr(f, "palatino") || strstr(f, "book") ||
            strstr(f, "cambria") || strstr(f, "baskerville") || strstr(f, "charter") ||
            strstr(f, "merriweather") || strstr(f, "serif") == f)
            return strstr(f, "sans") ? FAM_SANS : FAM_SERIF;
        if (strstr(f, "sans") || strstr(f, "arial") || strstr(f, "helvetica") ||
            strstr(f, "verdana") || strstr(f, "tahoma") || strstr(f, "system") ||
            strstr(f, "apple") || strstr(f, "segoe") || strstr(f, "roboto") || strstr(f, "ubuntu") ||
            strstr(f, "inter") || strstr(f, "trebuchet") || strstr(f, "lucida") ||
            strstr(f, "geneva") || strstr(f, "open") || strstr(f, "lato") || strstr(f, "noto") ||
            strstr(f, "ui-") == f)
            return FAM_SANS;
        if (!strcmp(f, "cursive") || !strcmp(f, "fantasy"))
            return FAM_SANS;
    }
    return n ? FAM_SANS : fallback;     /* web fonts we do not have: mostly sans */
}

static int font_size_value(const char *value, const struct style *parent, const struct vctx *v,
                           int *px)
{
    static const struct { const char *kw; int px; } kws[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
        { "x-large", 24 }, { "xx-large", 32 }, { "xxx-large", 48 },
    };
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (!strcasecmp(value, kws[i].kw))
            return *px = ZOOM(kws[i].px), 1;
    if (!strcasecmp(value, "smaller"))
        return *px = parent->font_size * 5 / 6, 1;
    if (!strcasecmp(value, "larger"))
        return *px = parent->font_size * 6 / 5, 1;
    struct vctx pv = *v;
    pv.font_size = parent->font_size;   /* em in font-size means the parent's */
    struct len l;
    if (!length_value(value, &pv, &l) || l.unit == U_AUTO)
        return 0;
    *px = len_on(l, parent->font_size);
    if (*px < 1)
        *px = 1;
    return 1;
}

static int weight_value(const char *w, int wl, int parent_bold, int *bold)
{
    if (word_is(w, wl, "bold") || word_is(w, wl, "bolder"))
        return *bold = 1, 1;
    if (word_is(w, wl, "normal") || word_is(w, wl, "lighter"))
        return *bold = 0, 1;
    if (wl == 3 && isdigit((unsigned char)w[0]) && w[1] == '0' && w[2] == '0')
        return *bold = w[0] >= '6', 1;
    (void)parent_bold;
    return 0;
}

static void apply_font_shorthand(struct style *st, const struct style *parent, const char *value,
                                 const struct vctx *v)
{
    const char *w[16];
    int wl[16], n = words(value, w, wl, 16), i = 0;
    st->bold = 0;
    st->italic = 0;
    for (; i < n; i++) {
        int b;
        if (word_is(w[i], wl[i], "italic") || word_is(w[i], wl[i], "oblique"))
            st->italic = 1;
        else if (weight_value(w[i], wl[i], parent->bold, &b))
            st->bold = (uint8_t)b;
        else if (word_is(w[i], wl[i], "normal") || word_is(w[i], wl[i], "small-caps"))
            ;
        else
            break;
    }
    if (i >= n)
        return;
    char size[64];
    const char *slash = memchr(w[i], '/', (size_t)wl[i]);
    int sl = slash ? (int)(slash - w[i]) : wl[i];
    if (sl >= (int)sizeof(size))
        return;
    memcpy(size, w[i], (size_t)sl);
    size[sl] = 0;
    int px;
    if (font_size_value(size, parent, v, &px))
        st->font_size = px;
    st->lh_kind = LH_NORMAL;
    if (slash) {
        char lh[32];
        int ll = wl[i] - sl - 1;
        if (ll > 0 && ll < (int)sizeof(lh)) {
            memcpy(lh, slash + 1, (size_t)ll);
            lh[ll] = 0;
            int num;
            int used = parse_num(lh, &num);
            if (used && !lh[used])
                st->lh_kind = LH_FACTOR, st->lh = num;
            else if (used) {
                struct vctx fv = *v;
                fv.font_size = st->font_size;
                struct len l;
                if (length_value(lh, &fv, &l))
                    st->lh_kind = LH_PX,
                    st->lh = len_on(l, st->font_size);
            }
        }
    }
    if (i + 1 < n)
        st->family = (uint8_t)family_of(w[i + 1], st->family);
}

/* ---- flex, grid and background helpers ---- */

/* justify-content, align-items and the rest: the last word decides
 * ("safe center", "first baseline"). */
static int align_value(const char *value, int *out)
{
    static const struct { const char *kw; uint8_t al; } kws[] = {
        { "auto", AL_AUTO }, { "normal", AL_NORMAL }, { "stretch", AL_STRETCH },
        { "start", AL_START }, { "flex-start", AL_START }, { "self-start", AL_START },
        { "left", AL_START }, { "end", AL_END }, { "flex-end", AL_END }, { "self-end", AL_END },
        { "right", AL_END }, { "center", AL_CENTER }, { "baseline", AL_BASELINE },
        { "space-between", AL_BETWEEN }, { "space-around", AL_AROUND },
        { "space-evenly", AL_EVENLY }, { "anchor-center", AL_ANCHOR_CENTER },
    };
    const char *w[4];
    int wl[4], n = words(value, w, wl, 4);
    if (!n)
        return 0;
    for (size_t i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if (word_is(w[n - 1], wl[n - 1], kws[i].kw))
            return *out = kws[i].al, 1;
    return 0;
}

/* The first of a value's words, NUL-terminated in buf. */
static const char *first_word(const char *value, char *buf, int size, const char **rest)
{
    const char *w[2];
    int wl[2], n = words(value, w, wl, 2);
    buf[0] = 0;
    if (n && wl[0] < size) {
        memcpy(buf, w[0], (size_t)wl[0]);
        buf[wl[0]] = 0;
    }
    if (rest)
        *rest = n > 1 ? w[1] : NULL;
    return buf;
}

/* ---- gradients ---- */

static void parse_position(const char *value, const struct vctx *v, struct len *px, struct len *py);

/* An angle ("45deg", "0.25turn", "1rad", "100grad", or a bare 0) in degrees; 0 if it is not one. */
static int angle_deg(const char *w, int wl, float *out)
{
    char t[64];
    if (wl <= 0 || wl >= (int)sizeof(t))
        return 0;
    snprintf(t, sizeof(t), "%.*s", wl, w);
    int num, used = parse_num(t, &num);
    if (!used)
        return 0;
    const char *end = t + used;
    double x = num / 100.0;
    if (!strcasecmp(end, "deg"))
        ;
    else if (!strcasecmp(end, "turn"))
        x *= 360;
    else if (!strcasecmp(end, "rad"))
        x *= 180 / M_PI;
    else if (!strcasecmp(end, "grad"))
        x *= 0.9;
    else if (*end || x != 0)
        return 0;
    *out = (float)x;
    return 1;
}

/* A stop position: a length or percentage (an angle for conic gradients, as a percentage of a turn). */
static int stop_position(const char *w, int wl, int conic, const struct vctx *v, struct len *out)
{
    char t[64];
    float deg;
    if (wl <= 0 || wl >= (int)sizeof(t))
        return 0;
    snprintf(t, sizeof(t), "%.*s", wl, w);
    if (conic && t[wl - 1] != '%' && angle_deg(w, wl, &deg)) {
        *out = (struct len){ (int)(deg * 10000 / 360 + (deg < 0 ? -0.5f : 0.5f)), 0, U_PCT };
        return 1;
    }
    return !isalpha((unsigned char)t[0]) && length_value(t, v, out) && out->unit != U_AUTO;
}

/* Side and corner keywords ("top", "bottom right") into x and y directions (-1, 0, 1). */
static int side_words(const char **w, const int *wl, int n, int *dx, int *dy)
{
    *dx = *dy = 0;
    for (int i = 0; i < n; i++) {
        if (word_is(w[i], wl[i], "left")) *dx = -1;
        else if (word_is(w[i], wl[i], "right")) *dx = 1;
        else if (word_is(w[i], wl[i], "top")) *dy = -1;
        else if (word_is(w[i], wl[i], "bottom")) *dy = 1;
        else if (!word_is(w[i], wl[i], "center")) return 0;
    }
    return n > 0;
}

/*
 * The first gradient function in a value: linear-gradient(), radial-gradient(), conic-gradient(),
 * their repeating-* forms, and the old -webkit-/-moz- ones (whose sides name where they start and
 * whose angles count anticlockwise from the right). current is currentColor. NULL if there is none.
 */
static const struct css_gradient *parse_gradient(struct arena *a, const char *value, const struct vctx *v,
                                                 uint32_t current)
{
    const char *g = strcasestr(value, "gradient(");
    if (!g || !a)
        return NULL;
    const char *name = g;
    while (name > value && (isalpha((unsigned char)name[-1]) || name[-1] == '-'))
        name--;
    int nl = (int)(g - name);
    char fn[48];
    snprintf(fn, sizeof(fn), "%.*s", nl < 47 ? nl : 47, name);
    for (char *q = fn; *q; q++)
        *q = (char)tolower((unsigned char)*q);
    const char *f = fn;
    int legacy = 0;
    if (!strncmp(f, "-webkit-", 8) || !strncmp(f, "-moz-", 5) || !strncmp(f, "-o-", 3) || !strncmp(f, "-ms-", 4)) {
        legacy = 1;
        f = strchr(f + 1, '-') + 1;
    }
    struct css_gradient *gr = arena_alloc(a, sizeof(*gr));
    if (!gr)
        return NULL;
    if (!strncmp(f, "repeating-", 10))
        gr->repeating = 1, f += 10;
    if (!strcmp(f, "linear-"))
        gr->kind = GK_LINEAR, gr->angle = 180;
    else if (!strcmp(f, "radial-"))
        gr->kind = GK_RADIAL;
    else if (!strcmp(f, "conic-"))
        gr->kind = GK_CONIC;
    else
        return NULL;
    gr->cx = gr->cy = (struct len){ 5000, 0, U_PCT };
    gr->rx.unit = gr->ry.unit = U_AUTO;

    const char *open = g + 8, *end = skip_group(open, open + strlen(open)) - 1, *p = open + 1;
    int conic = gr->kind == GK_CONIC;
    for (int arg = 0; p < end; arg++) {
        const char *e = find_top(p, end, ',');
        char buf[256];
        int len = (int)(e - p) < (int)sizeof(buf) - 1 ? (int)(e - p) : (int)sizeof(buf) - 1;
        memcpy(buf, p, (size_t)len);
        buf[len] = 0;
        p = e + 1;
        const char *w[8];
        int wl[8], n = words(buf, w, wl, 8);
        if (!n)
            continue;
        uint32_t col;
        /* the color may come first or (in old syntax) after its position */
        int ci = -1;
        for (int i = 0; i < n && ci < 0; i++)
            if (css_parse_color(w[i], wl[i], &col) == 0 || word_is(w[i], wl[i], "currentcolor")) {
                if (word_is(w[i], wl[i], "currentcolor"))
                    col = current;
                ci = i;
            }
        if (ci < 0 && arg == 0) {
            /* the gradient's shape: direction, size, centre */
            if (gr->kind == GK_LINEAR) {
                float deg;
                int dx, dy, has_to = word_is(w[0], wl[0], "to");
                if (angle_deg(w[0], wl[0], &deg)) {
                    gr->angle = legacy ? 90 - deg : deg;
                } else if (side_words(w + has_to, wl + has_to, n - has_to, &dx, &dy)) {
                    if (legacy && !has_to)
                        dx = -dx, dy = -dy;            /* the old syntax names where it starts */
                    if (dx && dy)
                        gr->corner_x = (int8_t)dx, gr->corner_y = (int8_t)dy;
                    else
                        gr->angle = dx > 0 ? 90 : dx < 0 ? 270 : dy < 0 ? 0 : 180;
                }
            } else {
                int at = -1;
                for (int i = 0; i < n; i++)
                    if (word_is(w[i], wl[i], "at"))
                        at = i;
                int shape_end = at >= 0 ? at : n;
                if (legacy && gr->kind == GK_RADIAL && at < 0) {
                    /* -webkit-radial-gradient(center, ellipse cover, ...): a position here, the shape next */
                    shape_end = 0;
                    at = -1;
                    char pos[256];
                    snprintf(pos, sizeof(pos), "%s", buf);
                    parse_position(pos, v, &gr->cx, &gr->cy);
                }
                int nlen = 0;
                for (int i = 0; i < shape_end; i++) {
                    float deg;
                    if (word_is(w[i], wl[i], "circle")) gr->circle = 1;
                    else if (word_is(w[i], wl[i], "ellipse")) gr->circle = 0;
                    else if (word_is(w[i], wl[i], "closest-side") || word_is(w[i], wl[i], "contain")) gr->extent = GX_CLOSEST_SIDE;
                    else if (word_is(w[i], wl[i], "closest-corner")) gr->extent = GX_CLOSEST_CORNER;
                    else if (word_is(w[i], wl[i], "farthest-side")) gr->extent = GX_FARTHEST_SIDE;
                    else if (word_is(w[i], wl[i], "farthest-corner") || word_is(w[i], wl[i], "cover")) gr->extent = GX_FARTHEST_CORNER;
                    else if (word_is(w[i], wl[i], "from") && i + 1 < shape_end && angle_deg(w[i + 1], wl[i + 1], &deg))
                        gr->angle = deg, i++;
                    else if (gr->kind == GK_RADIAL && nlen < 2) {
                        struct len l;
                        if (stop_position(w[i], wl[i], 0, v, &l))
                            *(nlen++ ? &gr->ry : &gr->rx) = l;
                    }
                }
                if (nlen == 1 && gr->rx.unit != U_PCT)
                    gr->circle = 1, gr->ry = gr->rx;
                else if (nlen == 2)
                    gr->circle = 0;
                if (at >= 0 && at + 1 < n) {
                    char pos[256];
                    int o = 0;
                    for (int i = at + 1; i < n && o < (int)sizeof(pos) - 70; i++)
                        o += snprintf(pos + o, sizeof(pos) - (size_t)o, "%.*s ", wl[i], w[i]);
                    parse_position(pos, v, &gr->cx, &gr->cy);
                }
            }
            continue;
        }
        if (ci < 0)
            continue;                   /* a colour hint on its own: the midpoint is taken as halfway */
        /* a stop, with up to two positions: "red", "red 10%", "red 10% 20%" (or "10% red") */
        int added = 0;
        for (int i = 0; i < n && gr->nstops < MAX_GSTOPS; i++) {
            if (i == ci)
                continue;
            struct len l;
            if (!stop_position(w[i], wl[i], conic, v, &l))
                continue;
            gr->stops[gr->nstops++] = (struct gstop){ col, l, 1 };
            added++;
        }
        if (!added && gr->nstops < MAX_GSTOPS)
            gr->stops[gr->nstops++] = (struct gstop){ col, { 0, 0, U_PX }, 0 };
    }
    if (gr->nstops == 0)
        return NULL;
    if (gr->nstops == 1)                /* one color: a plain fill */
        gr->stops[gr->nstops++] = gr->stops[0];
    return gr;
}

static const char *url_arg(struct arena *a, const char *text, const char *end, const char **after)
{
    for (const char *p = text; p + 4 < end; p++) {
        if (strncasecmp(p, "url(", 4) || (p > text && (isalnum((unsigned char)p[-1]) || p[-1] == '-')))
            continue;
        const char *q = p + 4;
        while (q < end && isspace((unsigned char)*q))
            q++;
        char quote = q < end && (*q == '"' || *q == '\'') ? *q++ : 0;
        const char *s = q;
        while (q < end && (quote ? *q != quote : *q != ')' && !isspace((unsigned char)*q))) {
            if (*q == '\\' && q + 1 < end)
                q++;
            q++;
        }
        const char *e = q;
        while (q < end && *q != ')')
            q++;
        if (after)
            *after = q < end ? q + 1 : end;
        if (!a || e == s)
            return NULL;
        return arena_strndup(a, s, (size_t)(e - s));
    }
    return NULL;
}

/* A background-position (or object-position) value into a point; one value leaves the other axis centered. */
static void parse_position(const char *value, const struct vctx *v, struct len *px, struct len *py)
{
    const char *w[4];
    int wl[4], n = words(value, w, wl, 4);
    struct len pos[2] = { { 5000, 0, U_PCT }, { 5000, 0, U_PCT } };
    int set[2] = { 0, 0 };
    if (!n)
        return;
    /* three or four words: edge keywords with offsets ("right 10px bottom 20px") */
    if (n >= 3) {
        for (int i = 0; i < n; i++) {
            int right = word_is(w[i], wl[i], "right"), bottom = word_is(w[i], wl[i], "bottom");
            int axis = right || word_is(w[i], wl[i], "left") ? 0 : bottom || word_is(w[i], wl[i], "top") ? 1 : -1;
            if (axis < 0)
                continue;
            struct len off = { 0, 0, U_PX };
            if (i + 1 < n) {
                char tmp[64];
                snprintf(tmp, sizeof(tmp), "%.*s", wl[i + 1], w[i + 1]);
                struct len l;
                if (word_is(w[i + 1], wl[i + 1], "center") || (!isalpha((unsigned char)tmp[0]) && length_value(tmp, v, &l) &&
                                                                  l.unit != U_AUTO)) {
                    if (!word_is(w[i + 1], wl[i + 1], "center"))
                        off = l;
                    i++;
                }
            }
            if (right || bottom) {
                pos[axis].unit = U_CALC;
                pos[axis].pct = 10000 - (off.unit == U_PCT ? off.v : 0);
                pos[axis].v = off.unit == U_PCT ? 0 : -off.v;
            } else {
                pos[axis] = off;
            }
            set[axis] = 1;
        }
        *px = pos[0], *py = pos[1];
        return;
    }
    int axis_of[2] = { -1, -1 };        /* 0: x, 1: y, -1: either */
    struct len each[2];
    int kinds[2];                       /* 0 keyword, 1 length */
    for (int i = 0; i < n && i < 2; i++) {
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.*s", wl[i], w[i]);
        kinds[i] = 0;
        if (word_is(w[i], wl[i], "left"))
            axis_of[i] = 0, each[i] = (struct len){ 0, 0, U_PCT };
        else if (word_is(w[i], wl[i], "right"))
            axis_of[i] = 0, each[i] = (struct len){ 10000, 0, U_PCT };
        else if (word_is(w[i], wl[i], "top"))
            axis_of[i] = 1, each[i] = (struct len){ 0, 0, U_PCT };
        else if (word_is(w[i], wl[i], "bottom"))
            axis_of[i] = 1, each[i] = (struct len){ 10000, 0, U_PCT };
        else if (word_is(w[i], wl[i], "center"))
            each[i] = (struct len){ 5000, 0, U_PCT };
        else if (length_value(tmp, v, &each[i]) && each[i].unit != U_AUTO)
            kinds[i] = 1;
        else
            return;                     /* not a position */
    }
    if (n == 1) {
        int ax = axis_of[0] < 0 ? 0 : axis_of[0];
        pos[ax] = each[0];
        if (axis_of[0] < 0 && !kinds[0])
            pos[1] = each[0];
    } else {
        int first = axis_of[0], second = axis_of[1];
        if (first == 1 || second == 0)  /* "top left", "bottom right": y first */
            pos[1] = each[0], pos[0] = each[1];
        else
            pos[0] = each[0], pos[1] = each[1];
    }
    (void)set;
    *px = pos[0], *py = pos[1];
}

/* One background layer (a comma-separated part of background): the picture and how it is placed. */
static void parse_bg_layer(struct style *st, const char *layer, const char *end, const struct vctx *v)
{
    char buf[1024];
    int o = 0;
    for (const char *p = layer; p < end && o < (int)sizeof(buf) - 4; p++) {
        if (!strncasecmp(p, "url(", 4) && (p == layer || !(isalnum((unsigned char)p[-1]) || p[-1] == '-'))) {
            p = skip_group(p + 3, end) - 1;     /* the picture is not a word of the layout */
            buf[o++] = ' ';
        } else if (*p == '(' || *p == '"' || *p == '\'') {      /* a function stays whole */
            const char *e = skip_group(p, end);
            int len = (int)(e - p);
            if (o + len >= (int)sizeof(buf) - 4)
                break;
            memcpy(buf + o, p, (size_t)len);
            o += len;
            p = e - 1;
        } else if (*p == '/') {
            buf[o++] = ' ', buf[o++] = '/', buf[o++] = ' ';
        } else {
            buf[o++] = *p;
        }
    }
    buf[o] = 0;
    const char *w[16];
    int wl[16], n = words(buf, w, wl, 16);
    char pos[256] = "", size[128] = "";
    int in_size = 0;
    int rep[2] = { 0, 0 }, nrep = 0, nbox = 0;
    for (int i = 0; i < n; i++) {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%.*s", wl[i], w[i]);
        if (wl[i] == 1 && w[i][0] == '/') {
            in_size = 1;
        } else if (!strncasecmp(tmp, "url(", 4) || strcasestr(tmp, "gradient(")) {
            continue;
        } else if (!strcasecmp(tmp, "repeat-x")) {
            st->bg_norep_x = 0, st->bg_norep_y = 1;
        } else if (!strcasecmp(tmp, "repeat-y")) {
            st->bg_norep_x = 1, st->bg_norep_y = 0;
        } else if (!strcasecmp(tmp, "no-repeat") || !strcasecmp(tmp, "repeat") || !strcasecmp(tmp, "space") ||
                   !strcasecmp(tmp, "round")) {
            if (nrep < 2)
                rep[nrep++] = !strcasecmp(tmp, "no-repeat");
            if (nrep == 1)
                st->bg_norep_x = st->bg_norep_y = (uint8_t)rep[0];
            else
                st->bg_norep_y = (uint8_t)rep[1];
        } else if (!strcasecmp(tmp, "border-box") || !strcasecmp(tmp, "content-box") || !strcasecmp(tmp, "padding-box")) {
            int box = !strcasecmp(tmp, "border-box") ? 0 : !strcasecmp(tmp, "padding-box") ? 1 : 2;
            if (!nbox++) st->bg_origin = box == 0 ? 1 : box == 1 ? 0 : 2;
            st->bg_clip = (uint8_t)box;
            in_size = 0;
        } else if (!strcasecmp(tmp, "scroll") || !strcasecmp(tmp, "fixed") || !strcasecmp(tmp, "local")) {
            st->bg_attachment = !strcasecmp(tmp, "fixed") ? 1 : !strcasecmp(tmp, "local") ? 2 : 0;
            in_size = 0;
        } else if (in_size) {
            if (strlen(size) + strlen(tmp) + 2 < sizeof(size))
                strcat(size, tmp), strcat(size, " ");
        } else {
            uint32_t col;
            if (css_parse_color(tmp, (int)strlen(tmp), &col) == 0)
                continue;               /* the color is the caller's */
            if (strlen(pos) + strlen(tmp) + 2 < sizeof(pos))
                strcat(pos, tmp), strcat(pos, " ");
        }
    }
    if (pos[0])
        parse_position(pos, v, &st->bg_px, &st->bg_py);
    if (size[0]) {
        const char *sw[2];
        int sl[2], sn = words(size, sw, sl, 2);
        char t0[64], t1[64] = "auto";
        snprintf(t0, sizeof(t0), "%.*s", sl[0], sw[0]);
        if (sn > 1)
            snprintf(t1, sizeof(t1), "%.*s", sl[1], sw[1]);
        if (!strcasecmp(t0, "cover"))
            st->bg_size = BGS_COVER;
        else if (!strcasecmp(t0, "contain"))
            st->bg_size = BGS_CONTAIN;
        else if (length_value(t0, v, &st->bg_w) && length_value(t1, v, &st->bg_h))
            st->bg_size = st->bg_w.unit == U_AUTO && st->bg_h.unit == U_AUTO ? BGS_AUTO : BGS_LEN;
    }
}

/* Everything about the background picture goes back to its defaults (the shorthand resets it). */
static void reset_bg_picture(struct style *st)
{
    st->bg_url = NULL;
    st->bg_size = BGS_AUTO;
    st->bg_w.unit = st->bg_h.unit = U_AUTO;
    st->bg_w.v = st->bg_h.v = 0;
    st->bg_px = st->bg_py = (struct len){ 0, 0, U_PX };
    st->bg_norep_x = st->bg_norep_y = 0;
    st->bg_origin = 0;
}

/* The first layer of a background value that has a url(), parsed into the style. */
static void apply_bg_layers(struct style *st, const char *value, const struct vctx *v)
{
    const char *end = value + strlen(value);
    for (const char *p = value; p <= end;) {
        const char *e = find_top(p, end, ',');
        if (e > end)
            e = end;
        const char *after;
        const char *url = url_arg(v->a, p, e, &after);
        if (url) {
            reset_bg_picture(st);
            st->bg_url = url;
            parse_bg_layer(st, p, e, v);
            return;
        }
        if (e >= end)
            break;
        p = e + 1;
    }
}

/* overflow: visible, hidden, clip, scroll or auto. */
static int overflow_kw(const char *w, int wl)
{
    return word_is(w, wl, "hidden") || word_is(w, wl, "clip") ? OV_HIDDEN
         : word_is(w, wl, "scroll") ? OV_SCROLL : word_is(w, wl, "auto") ? OV_AUTO : OV_VISIBLE;
}

/* One track size: 100px, 20%, 1fr, auto, min-content, max-content. */
static int track_size(const char *w, int wl, const struct vctx *v, uint8_t *kind, int *val)
{
    char buf[64];
    if (wl <= 0 || wl >= (int)sizeof(buf))
        return 0;
    memcpy(buf, w, (size_t)wl);
    buf[wl] = 0;
    *val = 0;
    if (!strcasecmp(buf, "auto") || !strncasecmp(buf, "fit-content", 11))
        return *kind = TK_AUTO, 1;
    if (!strcasecmp(buf, "min-content"))
        return *kind = TK_MIN, 1;
    if (!strcasecmp(buf, "max-content"))
        return *kind = TK_MAX, 1;
    int num, used = parse_num(buf, &num);
    if (used && !strcasecmp(buf + used, "fr"))
        return *kind = TK_FR, *val = num, 1;
    struct len l;
    if (!length_value(buf, v, &l) || l.unit == U_AUTO)
        return 0;
    *kind = l.unit == U_PCT ? TK_PCT : TK_PX;     /* min(100%, 250px) and calc() go by their pixels */
    *val = l.v;
    return 1;
}

static void add_track(struct gridspec *g, const char *w, int wl, const struct vctx *v)
{
    if (g->n >= MAX_TRACKS)
        return;
    struct track *t = &g->t[g->n];
    if (wl > 7 && !strncasecmp(w, "minmax(", 7)) {
        const char *in = w + 7, *end = w + wl - 1;
        const char *comma = find_top(in, end, ',');
        if (comma >= end)
            return;
        while (in < comma && isspace((unsigned char)*in))
            in++;
        const char *b = comma + 1;
        while (b < end && isspace((unsigned char)*b))
            b++;
        const char *ae = comma, *be = end;
        while (ae > in && isspace((unsigned char)ae[-1]))
            ae--;
        while (be > b && isspace((unsigned char)be[-1]))
            be--;
        if (!track_size(in, (int)(ae - in), v, &t->min, &t->vmin) ||
            !track_size(b, (int)(be - b), v, &t->max, &t->vmax))
            return;
        if (t->min == TK_FR)
            t->min = TK_AUTO;
    } else {
        if (!track_size(w, wl, v, &t->max, &t->vmax))
            return;
        t->min = t->max, t->vmin = t->vmax;
        if (t->max == TK_FR)                    /* 1fr is minmax(auto, 1fr) */
            t->min = TK_AUTO, t->vmin = 0;
    }
    g->n++;
}

static void add_line_names(struct gridspec *g, const char *s, int len, const struct vctx *v)
{
    if (len < 2 || *s != '[' || s[len-1] != ']') return;
    s++; len -= 2;
    const char *old = g->line_names[g->n];
    size_t size = (old ? strlen(old) : 0) + (size_t)len + 2;
    char *names = arena_alloc(v->a,size);
    if (!names) return;
    snprintf(names,size,"%s%.*s ",old ? old : "",len,s);
    g->line_names[g->n] = names;
}

/* grid-template-columns: "200px 1fr", "repeat(3, 1fr)", "repeat(auto-fill, minmax(180px, 1fr))". */
static const struct gridspec *parse_tracks(const char *value, const struct vctx *v)
{
    if (!v->a)
        return NULL;
    struct gridspec *g = arena_alloc(v->a, sizeof(*g));
    if (!g)
        return NULL;
    memset(g, 0, sizeof(*g));
    const char *w[64];
    int wl[64], n = words(value, w, wl, 64), in_names = 0;
    int sub = n && word_is(w[0], wl[0], "subgrid");
    g->subgrid = sub;
    for (int i = sub; i < n; i++) {
        if (in_names || w[i][0] == '[') {               /* [line names] */
            add_line_names(g,w[i],wl[i],v);
            in_names = w[i][wl[i] - 1] != ']';
            if(sub && !in_names && g->n<MAX_TRACKS)g->n++;
            continue;
        }
        if(sub) {
            if(wl[i]<10 || strncasecmp(w[i],"repeat(",7))return NULL;
            const char *comma=memchr(w[i]+7,',',(size_t)wl[i]-8);if(!comma)return NULL;
            char count_text[32];int cl=(int)(comma-w[i]-7);if(cl<=0 || cl>=32)return NULL;
            memcpy(count_text,w[i]+7,(size_t)cl);count_text[cl]=0;
            char *tail;long count=strtol(count_text,&tail,10);while(isspace((unsigned char)*tail))tail++;
            if(*tail || count<1 || count>MAX_TRACKS)return NULL;
            const char *part=comma+1,*end=w[i]+wl[i]-1;
            for(int repeat=0;repeat<count;repeat++) {
                const char *p=part;
                while(p<end) {
                    while(p<end && isspace((unsigned char)*p))p++;
                    if(p==end)break;
                    if(*p!='[')return NULL;
                    const char *close=memchr(p,']',(size_t)(end-p));if(!close || g->n>=MAX_TRACKS)return NULL;
                    add_line_names(g,p,(int)(close+1-p),v);g->n++;p=close+1;
                }
            }
            continue;
        }
        if (wl[i] > 7 && !strncasecmp(w[i], "repeat(", 7)) {
            char inner[256];
            int len = wl[i] - 8;
            if (len <= 0 || len >= (int)sizeof(inner))
                continue;
            memcpy(inner, w[i] + 7, (size_t)len);
            inner[len] = 0;
            const char *rw[32];
            int rwl[32], rn = words(inner, rw, rwl, 32);
            if (rn < 2)
                continue;
            int count = 1, autorep = 0;
            if (word_is(rw[0], rwl[0], "auto-fill"))
                autorep = 1;
            else if (word_is(rw[0], rwl[0], "auto-fit"))
                autorep = 2;
            else
                count = atoi(rw[0]);
            if (autorep && !g->auto_repeat) {
                g->auto_repeat = (uint8_t)autorep;
                g->rep = g->n;
            }
            for (int k = 0; k < (count < MAX_TRACKS ? count : MAX_TRACKS); k++) {
                int names = 0;
                for (int j = 1; j < rn; j++) {
                    if (names || rw[j][0] == '[') {
                        add_line_names(g,rw[j],rwl[j],v);
                        names = rw[j][rwl[j] - 1] != ']';
                        continue;
                    }
                    add_track(g, rw[j], rwl[j], v);
                }
            }
            if (autorep && g->auto_repeat == autorep && !g->nrep)
                g->nrep = g->n - g->rep;
            continue;
        }
        add_track(g, w[i], wl[i], v);
    }
    if(sub && in_names)return NULL;
    if (g->auto_repeat && !g->nrep)
        g->auto_repeat = 0;
    return g;
}

static int subgrid_supported(const char *value)
{
    struct arena a={0};const struct vctx v={16,16,1024,768,&a,0,0,NULL};
    const struct gridspec *g=parse_tracks(value,&v);
    int supported=g && g->subgrid;arena_free(&a);return supported;
}

/* Explicitly placed subgrids expose their parent's line names in local coordinates. */
static const struct gridspec *subgrid_names(struct arena *a,const struct gridspec *g,
                                           const struct gridspec *parent,int start,int end,int span)
{
    if(!g || !g->subgrid || !parent || !parent->n)return g;
    int first=start>0?start-1:start<0?parent->n+1+start:0;
    int last=end>0?end-1:end<0?parent->n+1+end:first+(span>0?span:1);
    if(first<0 || first>=parent->n || last<=first)return g;
    if(last>parent->n)last=parent->n;
    struct gridspec *out=arena_alloc(a,sizeof(*out));if(!out)return g;
    *out=*g;out->n=last-first;
    for(int k=0;k<=out->n;k++) {
        const char *local=g->line_names[k],*outer=parent->line_names[first+k];
        if(!outer)continue;
        size_t size=strlen(outer)+(local?strlen(local):0)+2;
        char *names=arena_alloc(a,size);
        if(names){snprintf(names,size,"%s %s",outer,local?local:"");out->line_names[k]=names;}
    }
    return out;
}

/* grid-template-areas: "header header" "nav main": each string is a row of cell names. */
static const struct gridareas *parse_areas(const char *value, const struct vctx *v)
{
    if (!v->a)
        return NULL;
    struct gridareas *g = arena_alloc(v->a, sizeof(*g));
    if (!g)
        return NULL;
    memset(g, 0, sizeof(*g));
    const char *p = value;
    int row = 0;
    while (*p) {
        while (*p && *p != '"' && *p != '\'')
            p++;
        if (!*p)
            break;
        char q = *p++;
        int col = 0;
        while (*p && *p != q) {
            while (*p == ' ' || *p == '\t')
                p++;
            const char *s = p;
            while (*p && *p != q && *p != ' ' && *p != '\t')
                p++;
            if (p == s)
                continue;
            if (!(p - s == 1 && *s == '.') && p - s < 24) {
                int k;
                for (k = 0; k < g->n; k++)
                    if (!strncmp(g->a[k].name, s, (size_t)(p - s)) && !g->a[k].name[p - s])
                        break;
                if (k == g->n && g->n < MAX_AREAS) {
                    memcpy(g->a[k].name, s, (size_t)(p - s));
                    g->a[k].r0 = row, g->a[k].c0 = col, g->a[k].r1 = row + 1, g->a[k].c1 = col + 1;
                    g->n++;
                } else if (k < g->n) {
                    if (row + 1 > g->a[k].r1)
                        g->a[k].r1 = row + 1;
                    if (col + 1 > g->a[k].c1)
                        g->a[k].c1 = col + 1;
                    if (col < g->a[k].c0)
                        g->a[k].c0 = col;
                }
            }
            col++;
        }
        if (*p)
            p++;
        if (col > g->ncols)
            g->ncols = col;
        row++;
    }
    g->nrows = row;
    return row ? g : NULL;
}

/* A grid line: "3", "-1", "span 2"; names and auto give 0. */
static void grid_line(const char *s, const char *end, int *line, int *span, const struct gridspec *g)
{
    char buf[64];
    while (s < end && isspace((unsigned char)*s))
        s++;
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    int len = (int)(end - s) < (int)sizeof(buf) - 1 ? (int)(end - s) : (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)len);
    buf[len] = 0;
    *line = 0;
    if (span)
        *span = 0;
    if (!strncasecmp(buf, "span", 4)) {
        if (span)
            *span = atoi(buf + 4) > 0 ? atoi(buf + 4) : 1;
        return;
    }
    if (isdigit((unsigned char)buf[0]) || buf[0] == '-')
        *line = atoi(buf);
    const char *w[3];
    int wl[3], n = words(buf,w,wl,3), occurrence = 1;
    const char *name = NULL;
    int name_len = 0;
    for (int i = 0; i < n; i++) {
        if (isdigit((unsigned char)w[i][0]) || w[i][0] == '-') occurrence = atoi(w[i]);
        else if (!word_is(w[i],wl[i],"auto")) name = w[i], name_len = wl[i];
    }
    if (!name || !g || !occurrence) return;
    *line = 0;
    long long remaining = occurrence > 0 ? occurrence : -(long long)occurrence;
    for (int i = occurrence > 0 ? 0 : g->n; i >= 0 && i <= g->n; i += occurrence > 0 ? 1 : -1) {
        const char *names = g->line_names[i];
        int found = 0;
        for (const char *p = names; p && *p;) {
            while (isspace((unsigned char)*p)) p++;
            const char *e = p;
            while (*e && !isspace((unsigned char)*e)) e++;
            if (e-p == name_len && !strncmp(p,name,(size_t)name_len)) found = 1;
            p = e;
        }
        if (found && !--remaining) { *line = i+1; return; }
    }
}

/* "start / end": a line and a span, or a line to a line. */
static void grid_placement(const char *value, int *start, int *end, int *span, const struct gridspec *g)
{
    const char *vend = value + strlen(value), *slash = find_top(value, vend, '/');
    int sspan, espan;
    grid_line(value, slash, start, &sspan, g);
    *end = 0;
    *span = sspan;
    if (slash < vend) {
        grid_line(slash + 1, vend, end, &espan, g);
        if (espan)
            *span = espan;
    }
}

static void set_len(struct len *dst, const char *value, const struct vctx *v)
{
    struct len l;
    if (!strcasecmp(value, "none"))
        dst->unit = U_AUTO, dst->v = 0;
    else if (length_value(value, v, &l))
        *dst = l;
}

/* width and height: a length, auto, or fit-content (min-content and max-content alike), which is auto
 * that shrinks to fit its contents where auto would stretch. */
static void set_size(struct len *dst, uint8_t *fit, const char *value, const struct vctx *v)
{
    if (!strncasecmp(value, "fit-content", 11) || !strncasecmp(value, "-moz-fit-content", 16) ||
        !strncasecmp(value, "-webkit-fit-content", 19) || !strcasecmp(value, "min-content") ||
        !strcasecmp(value, "max-content")) {
        dst->unit = U_AUTO, dst->v = dst->pct = 0;
        *fit = 1;
        return;
    }
    if (!strcasecmp(value, "initial") || !strcasecmp(value, "unset")) {
        dst->unit = U_AUTO, dst->v = dst->pct = 0;
        *fit = 0;
        return;
    }
    struct len before = *dst;
    set_len(dst, value, v);
    if (memcmp(&before, dst, sizeof(before)) || !strncasecmp(value, "auto", 4))
        *fit = 0;
}

/* Parse complete transform lists before changing the cascaded value. */
static int transform_number(const char *s, float *out, int angle)
{
    const char *end=s;
    int negative=0,digits=0;
    if(*end=='+' || *end=='-')negative=*end++=='-';
    double x=0;
    while(isdigit((unsigned char)*end)){x=x*10+(*end++-'0');digits++;}
    if(*end=='.') {
        double place=.1;end++;
        while(isdigit((unsigned char)*end)){x+=(*end++-'0')*place;place*=.1;digits++;}
    }
    if(!digits)return 0;
    if(*end=='e' || *end=='E') {
        const char *exponent=end+1;int eneg=0,power=0;
        if(*exponent=='+' || *exponent=='-')eneg=*exponent++=='-';
        if(isdigit((unsigned char)*exponent)) {
            while(isdigit((unsigned char)*exponent)){power=power*10+(*exponent++-'0');if(power>308)return 0;}
            while(power--)x*=eneg?.1:10;
            end=exponent;
        }
    }
    if(negative)x=-x;
    if (!__builtin_isfinite(x) || fabs(x) > 1000000) return 0;
    while (isspace((unsigned char)*end)) end++;
    if (angle) {
        if (!strcasecmp(end,"deg")) x *= 0.017453292519943295;
        else if (!strcasecmp(end,"grad")) x *= 0.015707963267948967;
        else if (!strcasecmp(end,"turn")) x *= 6.283185307179586;
        else if (strcasecmp(end,"rad") && (*end || x != 0)) return 0;
    } else if (*end) return 0;
    *out = (float)x;
    return 1;
}
/* Keep all three axes until the complete transform list has been composed. */
static int rotate3d_op(double x, double y, double z, double angle, struct tfop *op)
{
    double l = sqrt(x*x + y*y + z*z);
    memset(op, 0, sizeof(*op));
    op->kind = TF_MATRIX3D;
    op->m[0] = op->m[5] = op->m[10] = op->m[15] = 1;
    if (l <= 0) return 1;       /* a zero vector is the identity */
    x /= l; y /= l; z /= l;
    double c = cos(angle), s = sin(angle), t = 1-c;
    op->m[0]=t*x*x+c; op->m[1]=t*x*y+s*z; op->m[2]=t*x*z-s*y;
    op->m[4]=t*x*y-s*z; op->m[5]=t*y*y+c; op->m[6]=t*y*z+s*x;
    op->m[8]=t*x*z+s*y; op->m[9]=t*y*z-s*x; op->m[10]=t*z*z+c;
    op->flip = op->m[10] < -1e-6;
    return 1;
}
static int parse_transform(const char *value, const struct vctx *v, struct css_xform *out)
{
    memset(out,0,sizeof(*out));
    if (!strcasecmp(value,"none")) return 1;
    const char *p=value;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (out->n==MAX_TFOPS) return 0;
        const char *name=p;
        while (isalpha((unsigned char)*p) || isdigit((unsigned char)*p)) p++;
        int nl=(int)(p-name);
        if (*p!='(') return 0;
        const char *end=skip_group(p,p+strlen(p));
        if (end<=p+1 || end[-1]!=')') return 0;
        char args[16][128];int n=0;
        const char *q=p+1,*limit=end-1;
        while (q<limit) {
            while(q<limit && isspace((unsigned char)*q))q++;
            if(q==limit)break;
            const char *start=q;
            while(q<limit && *q!=',' && !isspace((unsigned char)*q)) {
                if(*q=='(')q=skip_group(q,limit);else q++;
            }
            if(n==16 || q-start<=0 || q-start>=(int)sizeof(args[0]))return 0;
            snprintf(args[n++],sizeof(args[0]),"%.*s",(int)(q-start),start);
            while(q<limit && isspace((unsigned char)*q))q++;
            if(q<limit && *q==','){q++;while(q<limit && isspace((unsigned char)*q))q++;if(q==limit)return 0;}
        }
        struct tfop op={0};op.m[10]=1;
        char fn[32];if(nl<=0 || nl>=(int)sizeof(fn))return 0;
        snprintf(fn,sizeof(fn),"%.*s",nl,name);
        if(!strcasecmp(fn,"translate") || !strcasecmp(fn,"translateX") || !strcasecmp(fn,"translateY")) {
            int axis=strcasecmp(fn,"translate")!=0;
            if(n<1 || n>(axis?1:2))return 0;
            op.kind=TF_TRANSLATE;
            if(!length_value(args[0],v,&op.x) || op.x.unit==U_AUTO)return 0;
            if(n==2 && (!length_value(args[1],v,&op.y) || op.y.unit==U_AUTO))return 0;
            if(!strcasecmp(fn,"translateY")){op.y=op.x;op.x=(struct len){0};}
        } else if(!strcasecmp(fn,"scale") || !strcasecmp(fn,"scaleX") || !strcasecmp(fn,"scaleY")) {
            int axis=strcasecmp(fn,"scale")!=0;
            if(n<1 || n>(axis?1:2) || !transform_number(args[0],&op.a,0))return 0;
            op.kind=TF_SCALE;op.b=op.a;
            if(n==2 && !transform_number(args[1],&op.b,0))return 0;
            if(!strcasecmp(fn,"scaleX"))op.b=1;
            if(!strcasecmp(fn,"scaleY")){op.b=op.a;op.a=1;}
        } else if(!strcasecmp(fn,"rotate")) {
            op.kind=TF_ROTATE;
            if(n!=1 || !transform_number(args[0],&op.a,1))return 0;
        } else if(!strcasecmp(fn,"skew") || !strcasecmp(fn,"skewX") || !strcasecmp(fn,"skewY")) {
            int axis=strcasecmp(fn,"skew")!=0;
            op.kind=TF_SKEW;
            if(n<1 || n>(axis?1:2) || !transform_number(args[0],&op.a,1))return 0;
            if(n==2 && !transform_number(args[1],&op.b,1))return 0;
            if(!strcasecmp(fn,"skewY")){op.b=op.a;op.a=0;}
        } else if(!strcasecmp(fn,"matrix")) {
            op.kind=TF_MATRIX;
            float values[6];if(n!=6)return 0;
            for(int i=0;i<6;i++)if(!transform_number(args[i],&values[i],0))return 0;
            op.a=values[0];op.b=values[1];op.c=values[2];op.d=values[3];op.e=values[4];op.f=values[5];
        /* Three-dimensional operations retain depth for perspective composition. */
        } else if(!strcasecmp(fn,"translate3d")) {
            if(n!=3 || !length_value(args[0],v,&op.x) || op.x.unit==U_AUTO || !length_value(args[1],v,&op.y) ||
               op.y.unit==U_AUTO || !length_value(args[2],v,&op.z) || op.z.unit!=U_PX)return 0;
            op.kind=TF_TRANSLATE;
        } else if(!strcasecmp(fn,"translateZ") || !strcasecmp(fn,"perspective")) {
            if(n!=1)return 0;
            op.kind=!strcasecmp(fn,"translateZ")?TF_TRANSLATE:TF_PERSPECTIVE;
            if(op.kind==TF_PERSPECTIVE && !strcasecmp(args[0],"none"))op.z.unit=U_AUTO;
            else if(!length_value(args[0],v,&op.z) || op.z.unit!=U_PX ||
                    (op.kind==TF_PERSPECTIVE && op.z.v<0))return 0;
        } else if(!strcasecmp(fn,"scale3d") || !strcasecmp(fn,"scaleZ")) {
            if(n!=(!strcasecmp(fn,"scale3d")?3:1))return 0;
            op.kind=TF_SCALE;op.a=op.b=1;
            if(n==3 && (!transform_number(args[0],&op.a,0) || !transform_number(args[1],&op.b,0)))return 0;
            if(!transform_number(args[n-1],&op.m[10],0))return 0;
        } else if(!strcasecmp(fn,"rotateZ")) {
            op.kind=TF_ROTATE;
            if(n!=1 || !transform_number(args[0],&op.a,1))return 0;
        } else if(!strcasecmp(fn,"rotateX") || !strcasecmp(fn,"rotateY") || !strcasecmp(fn,"rotate3d")) {
            float axis[3]={fn[6]=='X'||fn[6]=='x',fn[6]=='Y'||fn[6]=='y',0},angle;
            if(n!=(fn[6]=='3'?4:1))return 0;
            for(int i=0;i<3 && n==4;i++)if(!transform_number(args[i],&axis[i],0))return 0;
            if(!transform_number(args[n-1],&angle,1) || !rotate3d_op(axis[0],axis[1],axis[2],angle,&op))return 0;
        } else if(!strcasecmp(fn,"matrix3d")) {
            if(n!=16)return 0;
            op.kind=TF_MATRIX3D;
            for(int i=0;i<16;i++)if(!transform_number(args[i],&op.m[i],0))return 0;
            op.flip=op.m[10]<0;
        } else return 0;
        out->flipped^=op.flip;
        out->op[out->n++]=op;p=end;
    }
    return out->n>0;
}

/* Fractional percentages matter for transform origins and matrix translations. */
double css_transform_length(struct len l,int base)
{
    double p=(double)base*(l.unit==U_PCT?l.v:l.pct)/10000;
    if(l.unit==U_PCT)return p;
    if(l.unit==U_CALC)return l.v+p;
    if(l.unit==U_MIN)return p<l.v?p:l.v;
    if(l.unit==U_MAX)return p>l.v?p:l.v;
    return l.unit==U_AUTO?0:l.v;
}
void css_transform_matrix3d(const struct style *st,int w,int h,double out[16])
{
    memset(out,0,16*sizeof(double));out[0]=out[5]=out[10]=out[15]=1;
    for(int k=0;st && st->xf && k<st->xf->n;k++) {
        const struct tfop *op=&st->xf->op[k];
        double t[16]={0}, m[16]={0};t[0]=t[5]=t[10]=t[15]=1;
        switch(op->kind) {
        case TF_TRANSLATE:t[12]=css_transform_length(op->x,w);t[13]=css_transform_length(op->y,h);t[14]=css_transform_length(op->z,0);break;
        case TF_SCALE:t[0]=op->a;t[5]=op->b;t[10]=op->m[10];break;
        case TF_ROTATE:t[0]=t[5]=cos(op->a);t[1]=sin(op->a);t[4]=-t[1];break;
        case TF_SKEW:t[4]=tan(op->a);t[1]=tan(op->b);break;
        case TF_MATRIX:t[0]=op->a;t[1]=op->b;t[4]=op->c;t[5]=op->d;t[12]=op->e;t[13]=op->f;break;
        case TF_MATRIX3D:for(int i=0;i<16;i++)t[i]=op->m[i];break;
        case TF_PERSPECTIVE:if(op->z.unit!=U_AUTO)t[11]=-1.0/(op->z.v<1?1:op->z.v);break;
        }
        for(int col=0;col<4;col++)for(int row=0;row<4;row++)
            for(int j=0;j<4;j++)m[col*4+row]+=out[j*4+row]*t[col*4+j];
        memcpy(out,m,sizeof(m));
    }
}
void css_transform_matrix(const struct style *st,int w,int h,double out[6])
{
    double m[16];css_transform_matrix3d(st,w,h,m);
    out[0]=m[0];out[1]=m[1];out[2]=m[4];out[3]=m[5];out[4]=m[12];out[5]=m[13];
}
static int transform_supported(const char *value)
{
    const struct vctx v={16,16,1024,768,NULL, 0, 0, NULL };struct css_xform parsed;
    return parse_transform(value,&v,&parsed);
}

/*
 * The individual transform properties: translate: x [y [z]], rotate: angle (or about the z axis:
 * "z 45deg", "0 0 1 45deg"), scale: x [y [z]] (numbers or percentages). Returns 1 with *set 0 for none, 1 with *set 1 and op filled in, 0 for nonsense.
 */
static int individual_transform(int prop, const char *value, const struct vctx *v, struct tfop *op, int *set)
{
    const char *w[4];
    int wl[4], n = words(value, w, wl, 4);
    char a[3][64];
    memset(op, 0, sizeof(*op));
    *set = 0;
    if (n == 1 && word_is(w[0], wl[0], "none"))
        return 1;
    if (n < 1)
        return 0;
    for (int i = 0; i < n && i < 3; i++)
        snprintf(a[i], sizeof(a[i]), "%.*s", wl[i], w[i]);
    if (prop == P_TRANSLATE) {
        if (n > 3)
            return 0;
        op->kind = TF_TRANSLATE;
        if (!length_value(a[0], v, &op->x) || op->x.unit == U_AUTO)
            return 0;
        if (n >= 2 && (!length_value(a[1], v, &op->y) || op->y.unit == U_AUTO))
            return 0;
        if (n == 3 && (!length_value(a[2], v, &op->z) || op->z.unit != U_PX)) return 0;
    } else if (prop == P_ROTATE) {
        op->kind = TF_ROTATE;
        const char *angle = n == 1 ? a[0] : NULL;
        char last[64];
        snprintf(last, sizeof(last), "%.*s", wl[n - 1], w[n - 1]);
        if (n == 2 && (word_is(w[0], wl[0], "z")))
            angle = last;
        else if (n == 4 && word_is(w[0], wl[0], "0") && word_is(w[1], wl[1], "0") && word_is(w[2], wl[2], "1"))
            angle = last;
        else if ((n == 2 && (word_is(w[0], wl[0], "x") || word_is(w[0], wl[0], "y"))) || n == 4) {
            float axis[3] = { word_is(w[0], wl[0], "x"), word_is(w[0], wl[0], "y"), 0 }, turn;
            for (int i = 0; i < 3 && n == 4; i++) {
                char one[64];
                snprintf(one, sizeof(one), "%.*s", wl[i], w[i]);
                if (!transform_number(one, &axis[i], 0))
                    return 0;
            }
            if (!transform_number(last, &turn, 1) || !rotate3d_op(axis[0], axis[1], axis[2], turn, op))
                return 0;
            *set = 1;
            return 1;
        }
        if (!angle || !transform_number(angle, &op->a, 1))
            return 0;
    } else {
        if (n > 3)
            return 0;
        op->kind = TF_SCALE;
        op->m[10] = 1;
        float s[2];
        for (int i = 0; i < 2; i++) {
            const char *t = a[i < n ? i : 0];
            size_t tl = strlen(t);
            if (tl && t[tl - 1] == '%') {
                char num[64];
                snprintf(num, sizeof(num), "%.*s", (int)tl - 1, t);
                if (!transform_number(num, &s[i], 0))
                    return 0;
                s[i] /= 100;
            } else if (!transform_number(t, &s[i], 0)) {
                return 0;
            }
        }
        if (n == 3) {
            size_t len = strlen(a[2]);
            int pct = len && a[2][len-1]=='%';
            if (pct) a[2][len-1]=0;
            if (!transform_number(a[2], &op->m[10], 0)) return 0;
            if (pct) op->m[10]/=100;
        }
        op->a = s[0], op->b = s[1];
    }
    *set = 1;
    return 1;
}

static int individual_supported(int prop, const char *value)
{
    const struct vctx v = { 16, 16, 1024, 768, NULL, 0, 0, NULL };
    struct tfop op;
    int set;
    return individual_transform(prop, value, &v, &op, &set);
}

/* Put translate, rotate and scale before transform's own functions, in that order (as CSS has them). */
static void finish_transform(struct arena *a, struct style *st)
{
    if (!st->xf_ind || !a)
        return;
    struct css_xform *xf = arena_alloc(a, sizeof(*xf));
    if (!xf)
        return;
    if (st->xf_ind & 1)
        xf->op[xf->n++] = st->xf_translate;
    if (st->xf_ind & 2)
        xf->op[xf->n++] = st->xf_rotate;
    if (st->xf_ind & 4)
        xf->op[xf->n++] = st->xf_scale;
    for (int k = 0; st->xf && k < st->xf->n && xf->n < MAX_TFOPS; k++)
        xf->op[xf->n++] = st->xf->op[k];
    for (int k = 0; k < xf->n; k++)
        xf->flipped ^= xf->op[k].flip;
    st->xf = xf;
}

/* ---- filter, backdrop-filter, clip-path ---- */

/* The functions of a value, one at a time: "name(args)"; 0 at the end, -1 if it is not that. */
static int next_function(const char **pp, char *name, int nsize, const char **args, const char **args_end)
{
    const char *p = *pp;
    while (isspace((unsigned char)*p))
        p++;
    if (!*p)
        return 0;
    const char *n = p;
    while (isalnum((unsigned char)*p) || *p == '-')
        p++;
    if (*p != '(' || p == n || p - n >= nsize)
        return -1;
    snprintf(name, (size_t)nsize, "%.*s", (int)(p - n), n);
    const char *close = skip_group(p, p + strlen(p));
    if (close[-1] != ')')
        return -1;
    *args = p + 1, *args_end = close - 1;
    *pp = close;
    return 1;
}

/* An amount: "0.5", "50%" or nothing (def). */
static int fx_amount(const char *a, const char *e, float def, float *out)
{
    char t[64];
    while (a < e && isspace((unsigned char)*a))
        a++;
    while (e > a && isspace((unsigned char)e[-1]))
        e--;
    if (a == e) {
        *out = def;
        return 1;
    }
    if (e - a >= (int)sizeof(t))
        return 0;
    int pct = e[-1] == '%';
    snprintf(t, sizeof(t), "%.*s", (int)(e - a - pct), a);
    if (!transform_number(t, out, 0) || *out < 0)
        return 0;
    if (pct)
        *out /= 100;
    return 1;
}

/* drop-shadow(color? x y blur? color?) */
static int fx_shadow(const char *a, const char *e, const struct vctx *v, struct fxop *op)
{
    char buf[256];
    if (e - a >= (int)sizeof(buf))
        return 0;
    snprintf(buf, sizeof(buf), "%.*s", (int)(e - a), a);
    const char *w[6];
    int wl[6], n = words(buf, w, wl, 6), lens = 0;
    op->color = 1;                              /* currentColor until the cascade knows it */
    for (int i = 0; i < n; i++) {
        char t[128];
        snprintf(t, sizeof(t), "%.*s", wl[i], w[i]);
        struct len l;
        uint32_t c;
        if (lens < 3 && length_value(t, v, &l) && l.unit == U_PX) {
            int *slot[3] = { &op->dx, &op->dy, &op->blur };
            *slot[lens++] = l.v;
        } else if (css_parse_color(t, (int)strlen(t), &c) == 0) {
            op->color = c;
        } else {
            return 0;
        }
    }
    return lens >= 2 && op->blur >= 0;
}

/* filter: none, or blur() brightness() contrast() drop-shadow() grayscale() hue-rotate() invert()
 * opacity() saturate() sepia(). url() filters are not drawn: the value is taken as none. */
static int parse_filter(const char *value, const struct vctx *v, struct css_filter *out)
{
    memset(out, 0, sizeof(*out));
    if (!strcasecmp(value, "none"))
        return 1;
    const char *p = value, *a, *e;
    char fn[32];
    int r;
    while ((r = next_function(&p, fn, sizeof(fn), &a, &e)) > 0) {
        if (out->n == MAX_FXOPS)
            return 0;
        struct fxop *op = &out->op[out->n];
        memset(op, 0, sizeof(*op));
        static const struct { const char *name; uint8_t kind; float def; } amounts[] = {
            { "brightness", FX_BRIGHTNESS, 1 }, { "contrast", FX_CONTRAST, 1 }, { "grayscale", FX_GRAYSCALE, 1 },
            { "invert", FX_INVERT, 1 }, { "opacity", FX_OPACITY, 1 }, { "saturate", FX_SATURATE, 1 },
            { "sepia", FX_SEPIA, 1 },
        };
        int done = 0;
        for (unsigned i = 0; i < sizeof(amounts) / sizeof(amounts[0]) && !done; i++)
            if (!strcasecmp(fn, amounts[i].name)) {
                op->kind = amounts[i].kind;
                if (!fx_amount(a, e, amounts[i].def, &op->amount))
                    return 0;
                if (op->kind == FX_GRAYSCALE || op->kind == FX_INVERT || op->kind == FX_OPACITY || op->kind == FX_SEPIA)
                    op->amount = op->amount > 1 ? 1 : op->amount;
                done = 1;
            }
        if (done) {
        } else if (!strcasecmp(fn, "blur")) {
            char t[128];
            struct len l = { 0, 0, U_PX };
            snprintf(t, sizeof(t), "%.*s", (int)(e - a), a);
            if (t[strspn(t, " ")] && (!length_value(t, v, &l) || l.unit != U_PX || l.v < 0))
                return 0;
            op->kind = FX_BLUR;
            op->amount = (float)(l.v > 100 ? 100 : l.v);
        } else if (!strcasecmp(fn, "hue-rotate")) {
            char t[128];
            snprintf(t, sizeof(t), "%.*s", (int)(e - a), a);
            op->kind = FX_HUE;
            if (t[strspn(t, " ")] && !transform_number(t + strspn(t, " "), &op->amount, 1))
                return 0;
        } else if (!strcasecmp(fn, "drop-shadow")) {
            op->kind = FX_SHADOW;
            if (!fx_shadow(a, e, v, op))
                return 0;
            if (op->blur > 100)
                op->blur = 100;
        } else {
            return 0;
        }
        out->n++;
    }
    return r == 0;
}

/* One coordinate of a shape: a length, a percentage of the box, or a keyword for one. */
static int clip_len(const char *s, int sl, const struct vctx *v, struct len *out)
{
    char t[128];
    if (sl <= 0 || sl >= (int)sizeof(t))
        return 0;
    snprintf(t, sizeof(t), "%.*s", sl, s);
    if (!strcasecmp(t, "left") || !strcasecmp(t, "top"))
        return *out = (struct len){ 0, 0, U_PCT }, 1;
    if (!strcasecmp(t, "center"))
        return *out = (struct len){ 5000, 0, U_PCT }, 1;
    if (!strcasecmp(t, "right") || !strcasecmp(t, "bottom"))
        return *out = (struct len){ 10000, 0, U_PCT }, 1;
    return length_value(t, v, out) && out->unit != U_AUTO;
}

/* "at x y" after a circle or ellipse: the centre (the box's by default). */
static int clip_center(const char *at, const char *e, const struct vctx *v, struct css_clip *c)
{
    c->a[2] = c->a[3] = (struct len){ 5000, 0, U_PCT };
    if (!at)
        return 1;
    const char *w[4];
    int wl[4];
    char buf[256];
    snprintf(buf, sizeof(buf), "%.*s", (int)(e - at - 2), at + 2);
    int n = words(buf, w, wl, 4);
    if (n == 1) {
        if (!clip_len(w[0], wl[0], v, &c->a[2]))
            return 0;
        if (word_is(w[0], wl[0], "top") || word_is(w[0], wl[0], "bottom"))
            c->a[3] = c->a[2], c->a[2] = (struct len){ 5000, 0, U_PCT };
        return 1;
    }
    if (n != 2)
        return 0;
    int swap = word_is(w[0], wl[0], "top") || word_is(w[0], wl[0], "bottom") || word_is(w[1], wl[1], "left") ||
               word_is(w[1], wl[1], "right");
    return clip_len(w[swap], wl[swap], v, &c->a[2]) && clip_len(w[!swap], wl[!swap], v, &c->a[3]);
}

/* A radius: a length, closest-side or farthest-side. */
static int clip_radius(const char *s, int sl, const struct vctx *v, struct len *out, uint8_t *side)
{
    *side = 0;
    if (sl == 12 && !strncasecmp(s, "closest-side", 12))
        return *side = 1, 1;
    if (sl == 13 && !strncasecmp(s, "farthest-side", 13))
        return *side = 2, 1;
    return clip_len(s, sl, v, out);
}

/* clip-path: none, inset(), circle(), ellipse() or polygon(), with or without a box keyword. url()
 * and path() shapes are not drawn: the value is taken as none. */
static int parse_clip_path(const char *value, const struct vctx *v, struct css_clip *c)
{
    memset(c, 0, sizeof(*c));
    const char *p = value, *a, *e;
    char fn[32];
    while (isspace((unsigned char)*p))
        p++;
    if (!strncasecmp(p, "none", 4))
        return 1;
    /* a box keyword alone, or before or after the shape (always the border box here) */
    const char *paren = strchr(p, '(');
    if (!paren)
        return 1;
    const char *fs = paren;
    while (fs > p && (isalpha((unsigned char)fs[-1]) || fs[-1] == '-'))
        fs--;
    p = fs;
    if (next_function(&p, fn, sizeof(fn), &a, &e) <= 0)
        return 0;
    char buf[1024];
    if (e - a >= (int)sizeof(buf))
        return 0;
    snprintf(buf, sizeof(buf), "%.*s", (int)(e - a), a);
    const char *w[8];
    int wl[8], n;
    if (!strcasecmp(fn, "inset")) {
        c->kind = CLIP_INSET;
        char *round = strstr(buf, "round");
        if (round) {
            *round = 0;
            char r[128];
            snprintf(r, sizeof(r), "%s", round + 5);
            n = words(r, w, wl, 8);
            if (n < 1 || !clip_len(w[0], wl[0], v, &c->round))
                return 0;
        }
        n = words(buf, w, wl, 8);
        if (n < 1 || n > 4)
            return 0;
        for (int i = 0; i < n; i++)
            if (!clip_len(w[i], wl[i], v, &c->a[i]))
                return 0;
        if (n < 2) c->a[1] = c->a[0];
        if (n < 3) c->a[2] = c->a[0];
        if (n < 4) c->a[3] = c->a[1];
        return 1;
    }
    if (!strcasecmp(fn, "circle") || !strcasecmp(fn, "ellipse")) {
        int ellipse = fn[0] == 'e' || fn[0] == 'E';
        c->kind = ellipse ? CLIP_ELLIPSE : CLIP_CIRCLE;
        char *at = strstr(buf, "at ");
        if (at && at != buf && !isspace((unsigned char)at[-1]))
            at = NULL;
        const char *end = buf + strlen(buf);
        char radii[256];
        snprintf(radii, sizeof(radii), "%.*s", (int)((at ? at : end) - buf), buf);
        n = words(radii, w, wl, 8);
        c->rx_side = c->ry_side = 1;            /* closest-side unless given */
        if (n > (ellipse ? 2 : 1))
            return 0;
        if (n >= 1 && !clip_radius(w[0], wl[0], v, &c->a[0], &c->rx_side))
            return 0;
        if (ellipse && n == 2 && !clip_radius(w[1], wl[1], v, &c->a[1], &c->ry_side))
            return 0;
        if (ellipse && n == 1)
            return 0;
        return clip_center(at, end, v, c);
    }
    if (!strcasecmp(fn, "polygon")) {
        c->kind = CLIP_POLYGON;
        for (const char *q = buf; *q;) {
            const char *comma = strchr(q, ',');
            if (!comma)
                comma = q + strlen(q);
            char pt[256];
            snprintf(pt, sizeof(pt), "%.*s", (int)(comma - q), q);
            n = words(pt, w, wl, 8);
            if (n == 1 && (word_is(w[0], wl[0], "nonzero") || word_is(w[0], wl[0], "evenodd"))) {
            } else if (n != 2 || c->npts == MAX_CLIP_POINTS || !clip_len(w[0], wl[0], v, &c->px[c->npts]) ||
                       !clip_len(w[1], wl[1], v, &c->py[c->npts])) {
                return 0;
            } else {
                c->npts++;
            }
            q = *comma ? comma + 1 : comma;
        }
        return 1;                               /* under three points: nothing is left to see */
    }
    return 0;
}

static int effect_supported(int prop, const char *value)
{
    const struct vctx v = { 16, 16, 1024, 768, NULL, 0, 0, NULL };
    struct css_filter f;
    struct css_clip c;
    if (prop != P_CLIP_PATH)
        return parse_filter(value, &v, &f);
    return parse_clip_path(value, &v, &c) && (c.kind || !strcasecmp(value, "none"));
}

static int blend_mode(const char *value)
{
    static const char *names[] = { "normal", "multiply", "screen", "overlay", "darken", "lighten", "color-dodge",
                                   "color-burn", "hard-light", "soft-light", "difference", "exclusion" };
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcasecmp(value, names[i]))
            return (int)i;
    return BL_NORMAL;                           /* hue, saturation, color, luminosity: drawn normally */
}

/* The transition and animation properties, kept as written until finish_motion. */
static void apply_motion(struct style *st, const struct style *parent, int prop, const char *value,
                         int inherit_kw, int initial_kw, const struct vctx *v)
{
    if (!v->a)
        return;
    struct css_motion_raw *raw = arena_alloc(v->a, sizeof(*raw));
    if (!raw)
        return;
    if (st->raw_motion)
        *raw = *st->raw_motion;
    else
        memset(raw, 0, sizeof(*raw));
    st->raw_motion = raw;
    if (inherit_kw || initial_kw) {
        const struct css_motion_raw *from = inherit_kw ? parent->raw_motion : NULL;
        static const struct css_motion_raw none;
        if (!from)
            from = &none;
        if (prop == P_TRANSITION || prop == P_ANIMATION) {
            if (prop == P_TRANSITION) {
                raw->tprop = from->tprop, raw->tdur = from->tdur, raw->tdelay = from->tdelay;
                raw->tease = from->tease, raw->tbehav = from->tbehav;
            } else {
                const char *keep[5] = { raw->tprop, raw->tdur, raw->tdelay, raw->tease, raw->tbehav };
                *raw = *from;
                raw->tprop = keep[0], raw->tdur = keep[1], raw->tdelay = keep[2], raw->tease = keep[3];
                raw->tbehav = keep[4];
            }
            return;
        }
    }
    const char *val = arena_strdup(v->a, value);
    switch (prop) {
    case P_TRANSITION_PROPERTY: raw->tprop = val; return;
    case P_TRANSITION_DURATION: raw->tdur = val; return;
    case P_TRANSITION_TIMING: raw->tease = val; return;
    case P_TRANSITION_DELAY: raw->tdelay = val; return;
    case P_TRANSITION_BEHAVIOR: raw->tbehav = val; return;
    case P_ANIMATION_NAME: raw->aname = val; return;
    case P_ANIMATION_DURATION: raw->adur = val; return;
    case P_ANIMATION_TIMING: raw->aease = val; return;
    case P_ANIMATION_DELAY: raw->adelay = val; return;
    case P_ANIMATION_ITERATION: raw->aiter = val; return;
    case P_ANIMATION_DIRECTION: raw->adir = val; return;
    case P_ANIMATION_FILL: raw->afill = val; return;
    case P_ANIMATION_STATE: raw->astate = val; return;
    }
    /* A shorthand: split each comma-separated item into the longhands' lists. */
    enum { L_PROP, L_DUR, L_EASE, L_DELAY, L_NAME, L_ITER, L_DIR, L_FILL, L_STATE, L_BEHAV, NLISTS };
    char lists[NLISTS][512];
    int used[NLISTS] = { 0 };
    const char *end = value + strlen(value);
    for (const char *item = value; item <= end;) {
        const char *comma = find_top(item, end, ',');
        const char *part[NLISTS] = { 0 };
        int plen[NLISTS] = { 0 };
        const char *p = item, *t;
        int tl, times = 0;
        while ((tl = motion_token(&p, comma, &t)) > 0) {
            struct css_timing tmp;
            int ms, slot;
            if (parse_time(t, tl, &ms))
                slot = times++ ? L_DELAY : L_DUR;
            else if (timing_keyword(t, tl, &tmp))
                slot = L_EASE;
            else if (prop == P_TRANSITION)
                slot = (tl == 6 && !strncasecmp(t, "normal", 6)) || (tl == 14 && !strncasecmp(t, "allow-discrete", 14))
                           ? L_BEHAV : L_PROP;
            else if ((tl == 8 && !strncasecmp(t, "infinite", 8)) || isdigit((unsigned char)*t) || *t == '.')
                slot = L_ITER;
            else if ((tl == 7 && !strncasecmp(t, "reverse", 7)) || (tl == 9 && !strncasecmp(t, "alternate", 9)) ||
                     (tl == 17 && !strncasecmp(t, "alternate-reverse", 17)) ||
                     (tl == 6 && !strncasecmp(t, "normal", 6) && !part[L_DIR]))
                slot = L_DIR;
            else if ((tl == 8 && !strncasecmp(t, "forwards", 8)) || (tl == 9 && !strncasecmp(t, "backwards", 9)) ||
                     (tl == 4 && !strncasecmp(t, "both", 4)) ||
                     (tl == 4 && !strncasecmp(t, "none", 4) && part[L_NAME]))
                slot = L_FILL;
            else if ((tl == 7 && !strncasecmp(t, "running", 7)) || (tl == 6 && !strncasecmp(t, "paused", 6)))
                slot = L_STATE;
            else
                slot = L_NAME;
            if (slot >= 0 && !part[slot])
                part[slot] = t, plen[slot] = tl;
        }
        static const char *const defaults[NLISTS] = { "all", "0s", "ease", "0s", "none", "1", "normal", "none",
                                                      "running", "normal" };
        for (int i = 0; i < NLISTS; i++) {
            const char *x = part[i] ? part[i] : defaults[i];
            int xl = part[i] ? plen[i] : (int)strlen(defaults[i]);
            if (used[i] + xl + 3 < (int)sizeof(lists[i]))
                used[i] += snprintf(lists[i] + used[i], sizeof(lists[i]) - (size_t)used[i], "%s%.*s",
                                    used[i] ? ", " : "", xl, x);
        }
        if (comma >= end)
            break;
        item = comma + 1;
    }
    const char *out[NLISTS];
    for (int i = 0; i < NLISTS; i++)
        out[i] = arena_strdup(v->a, used[i] ? lists[i] : "");
    if (prop == P_TRANSITION) {
        raw->tprop = out[L_PROP], raw->tdur = out[L_DUR], raw->tease = out[L_EASE], raw->tdelay = out[L_DELAY];
        raw->tbehav = out[L_BEHAV];
    } else {
        raw->aname = out[L_NAME], raw->adur = out[L_DUR], raw->aease = out[L_EASE], raw->adelay = out[L_DELAY];
        raw->aiter = out[L_ITER], raw->adir = out[L_DIR], raw->afill = out[L_FILL], raw->astate = out[L_STATE];
    }
}

/* The n-th item (cycling) of a comma-separated list, or NULL for an empty or missing list. */
static const char *list_item(const char *list, int n, int *len)
{
    if (!list || !*list)
        return NULL;
    const char *end = list + strlen(list);
    int count = 1;
    for (const char *p = list; (p = find_top(p, end, ',')) < end; p++)
        count++;
    n %= count;
    const char *p = list;
    while (n--)
        p = find_top(p, end, ',') + 1;
    const char *e = find_top(p, end, ',');
    while (p < e && isspace((unsigned char)*p))
        p++;
    while (e > p && isspace((unsigned char)e[-1]))
        e--;
    *len = (int)(e - p);
    return p;
}

static int list_count(const char *list)
{
    if (!list || !*list)
        return 0;
    const char *end = list + strlen(list);
    int count = 1;
    for (const char *p = list; (p = find_top(p, end, ',')) < end; p++)
        count++;
    return count;
}

/* The written transition and animation lists into st->trans and st->anims. */
static void finish_motion(struct arena *a, struct style *st)
{
    const struct css_motion_raw *raw = st->raw_motion;
    st->trans = NULL, st->anims = NULL, st->ntrans = st->nanims = 0;
    if (!raw)
        return;
    int n = list_count(raw->tprop ? raw->tprop : (raw->tdur ? "all" : NULL));
    if (n > 32)
        n = 32;
    struct css_transition *tr = n ? arena_alloc(a, sizeof(*tr) * (size_t)n) : NULL;
    for (int i = 0; tr && i < n; i++) {
        int len, k = st->ntrans;
        const char *name = list_item(raw->tprop ? raw->tprop : "all", i, &len);
        memset(&tr[k], 0, sizeof(tr[k]));
        tr[k].props = anim_prop_bits(name, len);
        tr[k].name = arena_strndup(a, name, (size_t)len);
        const char *x;
        if ((x = list_item(raw->tdur, i, &len)) && !parse_time(x, len, &tr[k].duration))
            tr[k].duration = 0;
        if ((x = list_item(raw->tdelay, i, &len)) && !parse_time(x, len, &tr[k].delay))
            tr[k].delay = 0;
        if (!(x = list_item(raw->tease, i, &len)) || !timing_keyword(x, len, &tr[k].ease))
            timing_keyword("ease", 4, &tr[k].ease);
        if ((x = list_item(raw->tbehav, i, &len)))
            tr[k].discrete = len == 14 && !strncasecmp(x, "allow-discrete", 14);
        if (tr[k].props && tr[k].duration > 0 && tr[k].duration + tr[k].delay > 0)
            st->ntrans++;
    }
    st->trans = st->ntrans ? tr : NULL;
    n = list_count(raw->aname);
    if (n > 16)
        n = 16;
    struct css_animation *an = n ? arena_alloc(a, sizeof(*an) * (size_t)n) : NULL;
    for (int i = 0; an && i < n; i++) {
        int len, k = st->nanims;
        const char *name = list_item(raw->aname, i, &len), *x;
        if (len >= 2 && (*name == '"' || *name == '\''))
            name++, len -= 2;
        if (!len || (len == 4 && !strncasecmp(name, "none", 4)))
            continue;
        memset(&an[k], 0, sizeof(an[k]));
        an[k].name = arena_strndup(a, name, (size_t)len);
        an[k].iterations = 1;
        if ((x = list_item(raw->adur, i, &len)) && !parse_time(x, len, &an[k].duration))
            an[k].duration = 0;
        if ((x = list_item(raw->adelay, i, &len)) && !parse_time(x, len, &an[k].delay))
            an[k].delay = 0;
        if (!(x = list_item(raw->aease, i, &len)) || !timing_keyword(x, len, &an[k].ease))
            timing_keyword("ease", 4, &an[k].ease);
        if ((x = list_item(raw->aiter, i, &len)))
            an[k].iterations = !strncasecmp(x, "infinite", 8) ? -1 : strtof(x, NULL);
        if (an[k].iterations < -1)
            an[k].iterations = 1;
        if ((x = list_item(raw->adir, i, &len)))
            an[k].direction = !strncasecmp(x, "alternate-reverse", 17) ? AD_ALTERNATE_REVERSE
                            : !strncasecmp(x, "alternate", 9) ? AD_ALTERNATE
                            : !strncasecmp(x, "reverse", 7) ? AD_REVERSE : AD_NORMAL;
        if ((x = list_item(raw->afill, i, &len)))
            an[k].fill = !strncasecmp(x, "forwards", 8) ? AF_FORWARDS : !strncasecmp(x, "backwards", 9) ? AF_BACKWARDS
                       : !strncasecmp(x, "both", 4) ? AF_BOTH : AF_NONE;
        if ((x = list_item(raw->astate, i, &len)))
            an[k].paused = !strncasecmp(x, "paused", 6);
        st->nanims++;
    }
    st->anims = st->nanims ? an : NULL;
}

/* ---- anchor positioning: anchor(), anchor-size(), position-area, position-try-fallbacks ---- */

/* The element's anchor_spec, its own to change. */
static struct anchor_spec *anchor_spec_edit(struct style *st, struct arena *a)
{
    struct anchor_spec *sp = a ? arena_alloc(a, sizeof(*sp)) : NULL;
    if (!sp)
        return NULL;
    if (st->anchor)
        *sp = *st->anchor;
    st->anchor = sp;
    return sp;
}

/* anchor(--name side, fallback) (size 0) or anchor-size(--name width, fallback) (size 1), alone or
 * in calc() with pixels added or taken away: 1 if value is one, and *out says what it asks for. */
static int parse_aref(const char *value, int size, const struct vctx *v, struct aref *out)
{
    static const struct { const char *kw; int side; } sides[] = {
        { "top", AS_TOP }, { "bottom", AS_BOTTOM }, { "left", AS_LEFT }, { "right", AS_RIGHT },
        { "start", AS_START }, { "end", AS_END }, { "self-start", AS_START }, { "self-end", AS_END },
        { "center", AS_CENTER }, { "inside", AS_INSIDE }, { "outside", AS_OUTSIDE },
    }, sizes[] = {
        { "width", AS_WIDTH }, { "height", AS_HEIGHT }, { "inline", AS_WIDTH }, { "block", AS_HEIGHT },
        { "self-inline", AS_WIDTH }, { "self-block", AS_HEIGHT },
    };
    memset(out, 0, sizeof(*out));
    out->fallback.unit = U_AUTO;
    out->side = size ? AS_SAME : 5000;
    const char *end = value + strlen(value), *p = value;
    while (isspace((unsigned char)*p))
        p++;
    int calc = !strncasecmp(p, "calc(", 5);
    if (calc)
        for (p += 5; isspace((unsigned char)*p); p++)
            ;
    const char *fn = size ? "anchor-size(" : "anchor(";
    size_t fl = strlen(fn);
    if (strncasecmp(p, fn, fl))
        return 0;
    const char *close = skip_group(p + fl - 1, end), *a0 = p + fl;
    const char *a1 = close > a0 && close[-1] == ')' ? close - 1 : close, *comma = find_top(a0, a1, ',');
    int got_side = size;
    for (const char *q = a0; q < comma;) {
        while (q < comma && isspace((unsigned char)*q))
            q++;
        const char *w = q;
        while (q < comma && !isspace((unsigned char)*q))
            q++;
        int wl = (int)(q - w), known = 0;
        if (!wl)
            break;
        if (wl > 2 && w[0] == '-' && w[1] == '-') {
            out->name = v->a ? arena_strndup(v->a, w, (size_t)wl) : NULL;
            continue;
        }
        for (unsigned i = 0; !size && i < sizeof(sides) / sizeof(sides[0]); i++)
            if ((int)strlen(sides[i].kw) == wl && !strncasecmp(w, sides[i].kw, (size_t)wl))
                out->side = sides[i].side, known = got_side = 1;
        for (unsigned i = 0; size && i < sizeof(sizes) / sizeof(sizes[0]); i++)
            if ((int)strlen(sizes[i].kw) == wl && !strncasecmp(w, sizes[i].kw, (size_t)wl))
                out->side = sizes[i].side, known = 1;
        int pct;
        if (!known && !size && w[wl - 1] == '%' && parse_num(w, &pct)) {
            out->side = pct < 0 ? 0 : pct > 10000 ? 10000 : pct;
            known = got_side = 1;
        }
        if (!known)
            return 0;
    }
    if (!got_side)
        return 0;
    if (comma < a1) {
        char fb[128];
        snprintf(fb, sizeof(fb), "%.*s", (int)(a1 - comma - 1), comma + 1);
        if (!length_value(fb, v, &out->fallback))
            out->fallback.unit = U_AUTO;
    }
    if (calc) {                                 /* calc(anchor(bottom) + 4px) */
        const char *q = close;
        while (isspace((unsigned char)*q))
            q++;
        if (*q == '+' || *q == '-') {
            int sign = *q == '-' ? -1 : 1;
            char rest[64];
            snprintf(rest, sizeof(rest), "%.*s", (int)(end - q - 1), q + 1);
            char *paren = strrchr(rest, ')');
            if (paren)
                *paren = 0;
            struct len l;
            if (length_value(rest, v, &l) && l.unit == U_PX)
                out->add = sign * l.v;
        }
    }
    out->set = 1;
    return 1;
}

/* A position-area keyword: the axis it is for (0 either, 1 rows, 2 columns) and its lines. */
static int area_keyword(const char *w, int wl, int *axis, int *from, int *to)
{
    static const struct { const char *kw; uint8_t axis, from, to; } kws[] = {
        { "top", 1, 0, 1 }, { "bottom", 1, 2, 3 }, { "span-top", 1, 0, 2 }, { "span-bottom", 1, 1, 3 },
        { "y-start", 1, 0, 1 }, { "y-end", 1, 2, 3 }, { "span-y-start", 1, 0, 2 }, { "span-y-end", 1, 1, 3 },
        { "block-start", 1, 0, 1 }, { "block-end", 1, 2, 3 }, { "span-block-start", 1, 0, 2 },
        { "span-block-end", 1, 1, 3 }, { "self-block-start", 1, 0, 1 }, { "self-block-end", 1, 2, 3 },
        { "left", 2, 0, 1 }, { "right", 2, 2, 3 }, { "span-left", 2, 0, 2 }, { "span-right", 2, 1, 3 },
        { "x-start", 2, 0, 1 }, { "x-end", 2, 2, 3 }, { "span-x-start", 2, 0, 2 }, { "span-x-end", 2, 1, 3 },
        { "inline-start", 2, 0, 1 }, { "inline-end", 2, 2, 3 }, { "span-inline-start", 2, 0, 2 },
        { "span-inline-end", 2, 1, 3 }, { "self-inline-start", 2, 0, 1 }, { "self-inline-end", 2, 2, 3 },
        { "center", 0, 1, 2 }, { "start", 0, 0, 1 }, { "end", 0, 2, 3 }, { "self-start", 0, 0, 1 },
        { "self-end", 0, 2, 3 }, { "span-start", 0, 0, 2 }, { "span-end", 0, 1, 3 }, { "span-all", 0, 0, 3 },
    };
    for (unsigned i = 0; i < sizeof(kws) / sizeof(kws[0]); i++)
        if ((int)strlen(kws[i].kw) == wl && !strncasecmp(w, kws[i].kw, (size_t)wl)) {
            *axis = kws[i].axis, *from = kws[i].from, *to = kws[i].to;
            return 1;
        }
    return 0;
}

/* position-area: none, or one or two keywords (rows, columns: see anchor_spec). */
static int parse_area(const char *value, uint8_t out[4])
{
    const char *w[3];
    int wl[3], n = words(value, w, wl, 3), axis[2], from[2], to[2];
    memset(out, 0, 4);
    if (n == 1 && word_is(w[0], wl[0], "none"))
        return 1;
    if (n < 1 || n > 2)
        return 0;
    for (int i = 0; i < n; i++)
        if (!area_keyword(w[i], wl[i], &axis[i], &from[i], &to[i]))
            return 0;
    int r0, r1, c0, c1;
    if (n == 1) {
        r0 = axis[0] == 2 ? 0 : from[0], r1 = axis[0] == 2 ? 3 : to[0];
        c0 = axis[0] == 1 ? 0 : from[0], c1 = axis[0] == 1 ? 3 : to[0];
    } else {
        if (axis[0] && axis[0] == axis[1])
            return 0;
        int cols_first = axis[0] == 2 || axis[1] == 1;
        r0 = from[cols_first], r1 = to[cols_first], c0 = from[!cols_first], c1 = to[!cols_first];
    }
    out[0] = (uint8_t)r0, out[1] = (uint8_t)r1, out[2] = (uint8_t)c0, out[3] = (uint8_t)c1;
    return 1;
}

/* position-try-fallbacks: flip-block, flip-inline, flip-start (or several), and position-area values. */
static void parse_tries(const char *value, struct anchor_spec *sp)
{
    sp->ntry = 0;
    const char *end = value + strlen(value);
    for (const char *item = value; item < end && sp->ntry < MAX_TRIES;) {
        const char *comma = find_top(item, end, ',');
        char one[96];
        snprintf(one, sizeof(one), "%.*s", (int)(comma - item), item);
        const char *w[3];
        int wl[3], n = words(one, w, wl, 3), flip = 0, flips = 1;
        for (int i = 0; i < n; i++) {
            int f = word_is(w[i], wl[i], "flip-block") ? TRY_BLOCK : word_is(w[i], wl[i], "flip-inline") ? TRY_INLINE
                  : word_is(w[i], wl[i], "flip-start") ? TRY_START : 0;
            flip |= f;
            flips &= f != 0;
        }
        uint8_t area[4];
        const char *a = one;
        while (isspace((unsigned char)*a))
            a++;
        if (!strncasecmp(a, "position-area(", 14)) {     /* the older spelling */
            a += 14;
            char *paren = strrchr(one, ')');
            if (paren)
                *paren = 0;
        }
        if (n && flips) {
            sp->tries[sp->ntry].flip = (uint8_t)flip;
            memset(sp->tries[sp->ntry++].area, 0, 4);
        } else if (parse_area(a, area) && area[1]) {
            sp->tries[sp->ntry].flip = 0;
            memcpy(sp->tries[sp->ntry++].area, area, 4);
        }
        item = comma < end ? comma + 1 : end;
    }
}

static void apply(struct style *st, const struct style *parent, int prop, const char *value,
                  const struct vctx *v)
{
    const char *w[8];
    int wl[8], n;
    uint32_t c;
    struct len box[4];
    int inherit_kw = !strcasecmp(value, "inherit");
    int initial_kw = !strcasecmp(value, "initial") || !strcasecmp(value, "unset") ||
                     !strcasecmp(value, "revert") || !strcasecmp(value, "revert-layer");

    static const int backgrounds[] = { P_BACKGROUND, P_BACKGROUND_IMAGE, P_BACKGROUND_SIZE,
        P_BACKGROUND_POSITION, P_BACKGROUND_REPEAT, P_BACKGROUND_ORIGIN, P_BACKGROUND_CLIP,
        P_BACKGROUND_ATTACHMENT, P_BACKGROUND_POSITION_X, P_BACKGROUND_POSITION_Y };
    for (unsigned k = 0; k < sizeof(backgrounds)/sizeof(backgrounds[0]); k++)
        if (prop == backgrounds[k]) {
            if (!k) memset(st->bg_values, 0, sizeof(st->bg_values));
            st->bg_values[k] = v->a ? arena_strdup(v->a, value) : value;
            break;
        }

    /* anchor() in an inset, anchor-size() in a size: the length gets what applies when there is no
     * such anchor, and the reference goes beside it, for layout.c to measure the anchor. */
    int slot = prop == P_TOP ? 0 : prop == P_RIGHT ? 1 : prop == P_BOTTOM ? 2 : prop == P_LEFT ? 3
             : prop == P_WIDTH ? 4 : prop == P_HEIGHT ? 5 : -1;
    if (slot >= 0) {
        struct aref r;
        int ref = strcasestr(value, "anchor") && parse_aref(value, slot >= 4, v, &r);
        const struct aref *old = !st->anchor ? NULL : slot < 4 ? &st->anchor->inset[slot] : &st->anchor->size[slot - 4];
        if (ref || (old && old->set)) {
            struct anchor_spec *sp = anchor_spec_edit(st, v->a);
            struct aref *dst = !sp ? NULL : slot < 4 ? &sp->inset[slot] : &sp->size[slot - 4];
            if (dst) {
                memset(dst, 0, sizeof(*dst));
                if (ref)
                    *dst = r;
            }
        }
        if (ref) {
            struct len *len = slot == 0 ? &st->top : slot == 1 ? &st->right : slot == 2 ? &st->bottom
                            : slot == 3 ? &st->left : slot == 4 ? &st->width : &st->height;
            *len = r.fallback;
            if (slot == 4)
                st->fit_w = 0;
            if (slot == 5)
                st->fit_h = 0;
            return;
        }
    }
    if (prop == P_INSET && st->anchor && (st->anchor->inset[0].set || st->anchor->inset[1].set ||
                                          st->anchor->inset[2].set || st->anchor->inset[3].set)) {
        struct anchor_spec *sp = anchor_spec_edit(st, v->a);
        if (sp)
            memset(sp->inset, 0, sizeof(sp->inset));
    }

    switch (prop) {
    case P_DISPLAY:
        if (inherit_kw) {
            st->display = parent->display;
            st->inner = parent->inner;
        } else {
            static const struct { const char *kw; uint8_t d, in; } ds[] = {
                { "none", D_NONE, 0 }, { "block", D_BLOCK, 0 }, { "inline", D_INLINE, 0 },
                { "inline-block", D_INLINE_BLOCK, 0 }, { "list-item", D_LIST_ITEM, 0 },
                { "table", D_TABLE, 0 }, { "inline-table", D_INLINE_TABLE, 0 },
                { "table-row", D_TABLE_ROW, 0 }, { "table-row-group", D_TABLE_ROW_GROUP, 0 },
                { "table-header-group", D_TABLE_ROW_GROUP, 0 },
                { "table-footer-group", D_TABLE_ROW_GROUP, 0 }, { "table-cell", D_TABLE_CELL, 0 },
                { "table-caption", D_TABLE_CAPTION, 0 }, { "table-column", D_NONE, 0 },
                { "table-column-group", D_NONE, 0 }, { "flow-root", D_BLOCK, 0 },
                { "contents", D_INLINE, 0 }, { "run-in", D_BLOCK, 0 }, { "-webkit-box", D_BLOCK, 0 },
                { "inline flow-root", D_INLINE_BLOCK, 0 },
                { "flex", D_BLOCK, IN_FLEX }, { "block flex", D_BLOCK, IN_FLEX },
                { "-ms-flexbox", D_BLOCK, IN_FLEX }, { "-webkit-flex", D_BLOCK, IN_FLEX },
                { "inline-flex", D_INLINE_BLOCK, IN_FLEX }, { "inline flex", D_INLINE_BLOCK, IN_FLEX },
                { "-webkit-inline-flex", D_INLINE_BLOCK, IN_FLEX },
                { "-ms-inline-flexbox", D_INLINE_BLOCK, IN_FLEX },
                { "grid", D_BLOCK, IN_GRID }, { "block grid", D_BLOCK, IN_GRID },
                { "inline-grid", D_INLINE_BLOCK, IN_GRID }, { "inline grid", D_INLINE_BLOCK, IN_GRID },
                /* ruby is drawn as the user agent sheet sets <ruby> up: an inline flex column */
                { "ruby", D_INLINE_BLOCK, IN_FLEX }, { "ruby-text", D_BLOCK, 0 }, { "ruby-base", D_BLOCK, 0 },
                { "ruby-base-container", D_BLOCK, 0 }, { "ruby-text-container", D_BLOCK, 0 },
                { "inline list-item", D_LIST_ITEM, 0 }, { "block flow", D_BLOCK, 0 }, { "inline flow", D_INLINE, 0 },
                { "block flow-root", D_BLOCK, 0 }, { "inline-list-item", D_LIST_ITEM, 0 },
            };
            for (size_t i = 0; i < sizeof(ds) / sizeof(ds[0]); i++)
                if (!strcasecmp(value, ds[i].kw)) {
                    st->display = ds[i].d;
                    st->inner = ds[i].in;
                }
        }
        break;
    case P_FLEX_DIRECTION:
    case P_FLEX_FLOW:
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++) {
            if (!strncasecmp(w[i], "row", 3) || !strncasecmp(w[i], "column", 6)) {
                st->flex_column = !strncasecmp(w[i], "column", 6);
                st->flex_reverse = wl[i] > 8 && !strncasecmp(w[i] + wl[i] - 8, "-reverse", 8);
            } else if (word_is(w[i], wl[i], "wrap") || word_is(w[i], wl[i], "wrap-reverse")) {
                st->flex_wrap = 1;
            } else if (word_is(w[i], wl[i], "nowrap")) {
                st->flex_wrap = 0;
            }
        }
        break;
    case P_FLEX_WRAP:
        st->flex_wrap = !strncasecmp(value, "wrap", 4);
        break;
    case P_FLEX:
        if (!strcasecmp(value, "none")) {
            st->flex_grow = st->flex_shrink = 0;
            st->flex_basis.unit = U_AUTO;
        } else if (!strcasecmp(value, "auto")) {
            st->flex_grow = st->flex_shrink = 100;
            st->flex_basis.unit = U_AUTO;
        } else if (initial_kw) {
            st->flex_grow = 0, st->flex_shrink = 100;
            st->flex_basis.unit = U_AUTO;
        } else {
            int nums = 0, num, basis = 0;
            n = words(value, w, wl, 8);
            for (int i = 0; i < n; i++) {
                int used = parse_num(w[i], &num);
                if (used == wl[i] && nums < 2 && !basis) {
                    if (nums++ == 0)
                        st->flex_grow = num > 0 ? num : 0;
                    else
                        st->flex_shrink = num > 0 ? num : 0;
                } else {
                    char tmp[64];
                    snprintf(tmp, sizeof(tmp), "%.*s", wl[i], w[i]);
                    struct len l;
                    if (!strcasecmp(tmp, "content"))
                        st->flex_basis.unit = U_AUTO, basis = 1;
                    else if (length_value(tmp, v, &l))
                        st->flex_basis = l, basis = 1;
                }
            }
            if (nums == 1 || (!nums && basis))     /* flex: 2 and flex: 200px */
                st->flex_shrink = 100;
            if (!nums && basis)
                st->flex_grow = 100;
            if (nums && !basis)                 /* flex: 1 is 1 1 0% */
                st->flex_basis = (struct len){ .unit = U_PX };
        }
        break;
    case P_FLEX_GROW:
    case P_FLEX_SHRINK: {
        int num;
        if (parse_num(value, &num) && num >= 0)
            *(prop == P_FLEX_GROW ? &st->flex_grow : &st->flex_shrink) = num;
        else if (initial_kw)
            *(prop == P_FLEX_GROW ? &st->flex_grow : &st->flex_shrink) = prop == P_FLEX_GROW ? 0 : 100;
        break;
    }
    case P_FLEX_BASIS:
        if (!strcasecmp(value, "content") || initial_kw)
            st->flex_basis.unit = U_AUTO;
        else
            set_len(&st->flex_basis, value, v);
        break;
    case P_JUSTIFY_CONTENT:
    case P_ALIGN_CONTENT:
    case P_ALIGN_ITEMS:
    case P_ALIGN_SELF:
    case P_JUSTIFY_ITEMS:
    case P_JUSTIFY_SELF: {
        int al;
        if (!align_value(value, &al))
            break;
        switch (prop) {
        case P_JUSTIFY_CONTENT: st->justify = (uint8_t)al; break;
        case P_ALIGN_CONTENT: st->align_content = (uint8_t)al; break;
        case P_ALIGN_ITEMS: st->align_items = (uint8_t)al; break;
        case P_ALIGN_SELF: st->align_self = (uint8_t)al; break;
        case P_JUSTIFY_ITEMS: st->justify_items = (uint8_t)al; break;
        default: st->justify_self = (uint8_t)al; break;
        }
        break;
    }
    case P_PLACE_CONTENT:
    case P_PLACE_ITEMS:
    case P_PLACE_SELF: {
        /* align, then justify (or the same for both) */
        char first[64];
        const char *rest;
        int a1, a2;
        first_word(value, first, sizeof(first), &rest);
        if (!align_value(first, &a1))
            break;
        if (!rest || !align_value(rest, &a2))
            a2 = a1;
        if (prop == P_PLACE_CONTENT)
            st->align_content = (uint8_t)a1, st->justify = (uint8_t)a2;
        else if (prop == P_PLACE_ITEMS)
            st->align_items = (uint8_t)a1, st->justify_items = (uint8_t)a2;
        else
            st->align_self = (uint8_t)a1, st->justify_self = (uint8_t)a2;
        break;
    }
    case P_ORDER: {
        int num;
        if (parse_num(value, &num))
            st->order = num / 100;
        break;
    }
    case P_GAP:
    case P_ROW_GAP:
    case P_COLUMN_GAP: {
        struct len l[2] = { { .unit = U_PX }, { .unit = U_PX } };
        int normal[2] = { 1, 1 };
        n = words(value, w, wl, 8);
        for (int i = 0; i < n && i < 2; i++) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.*s", wl[i], w[i]);
            if (!length_value(tmp, v, &l[i]) || l[i].unit == U_AUTO)
                l[i] = (struct len){ .unit = U_PX };    /* normal */
            else
                normal[i] = 0;
        }
        if (n == 1)
            l[1] = l[0], normal[1] = normal[0];
        if (prop != P_COLUMN_GAP) {
            st->row_gap = l[0];
            st->row_gap_normal = normal[0];
        }
        if (prop != P_ROW_GAP) {
            st->col_gap = prop == P_GAP ? l[1] : l[0];
            st->col_gap_normal = (uint8_t)(prop == P_GAP ? normal[1] : normal[0]);
        }
        break;
    }
    case P_COLUMNS:
    case P_COLUMN_COUNT:
    case P_COLUMN_WIDTH: {                      /* "3", "12em", "3 12em", "auto" */
        if (prop != P_COLUMN_WIDTH)
            st->col_count = 0;
        if (prop != P_COLUMN_COUNT)
            st->col_width = (struct len){ .unit = U_AUTO };
        if (inherit_kw) {
            if (prop != P_COLUMN_WIDTH)
                st->col_count = parent->col_count;
            if (prop != P_COLUMN_COUNT)
                st->col_width = parent->col_width;
            break;
        }
        n = words(value, w, wl, 2);
        for (int i = 0; i < n; i++) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.*s", wl[i], w[i]);
            struct len l;
            const char *e = tmp;
            while (isdigit((unsigned char)*e))
                e++;
            if (*e == 0 && e > tmp && prop != P_COLUMN_WIDTH) {
                int k = atoi(tmp);
                st->col_count = (uint8_t)(k > 255 ? 255 : k);
            } else if (prop != P_COLUMN_COUNT && length_value(tmp, v, &l) && l.unit == U_PX && l.v > 0) {
                st->col_width = l;
            }
        }
        break;
    }
    case P_COLUMN_RULE:
    case P_COLUMN_RULE_WIDTH:
    case P_COLUMN_RULE_STYLE:
    case P_COLUMN_RULE_COLOR: {
        if (prop == P_COLUMN_RULE || initial_kw) {
            st->col_rule_style = BS_NONE;
            st->col_rule_w = ZOOM(3);
            st->col_rule_color = 1;
            if (initial_kw)
                break;
        }
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++) {
            int bs = parse_border_style(w[i], wl[i]), px;
            if (bs >= 0 && (prop == P_COLUMN_RULE || prop == P_COLUMN_RULE_STYLE))
                st->col_rule_style = (uint8_t)bs;
            else if ((prop == P_COLUMN_RULE || prop == P_COLUMN_RULE_WIDTH) && border_width_kw(w[i], wl[i], v, &px))
                st->col_rule_w = px;
            else if ((prop == P_COLUMN_RULE || prop == P_COLUMN_RULE_COLOR) && css_parse_color(w[i], wl[i], &c) == 0)
                st->col_rule_color = c;
            else if ((prop == P_COLUMN_RULE || prop == P_COLUMN_RULE_COLOR) && word_is(w[i], wl[i], "currentcolor"))
                st->col_rule_color = 1;
        }
        break;
    }
    case P_COLUMN_FILL:
        st->col_fill_auto = !strcasecmp(value, "auto");
        break;
    case P_BREAK_INSIDE:                        /* avoid, avoid-column, avoid-page */
        st->break_avoid = !strncasecmp(value, "avoid", 5);
        break;
    case P_BREAK_BEFORE:
    case P_BREAK_AFTER: {                       /* only column breaks: the page is not paginated */
        int forced = !strcasecmp(value, "column") || !strcasecmp(value, "always");
        if (prop == P_BREAK_BEFORE)
            st->break_before = (uint8_t)forced;
        else
            st->break_after = (uint8_t)forced;
        break;
    }
    case P_OVERFLOW: {
        n = words(value, w, wl, 2);
        st->ov_x = st->ov_y = OV_VISIBLE;
        if (n && !inherit_kw && !initial_kw) {
            st->ov_x = (uint8_t)overflow_kw(w[0], wl[0]);
            st->ov_y = n > 1 ? (uint8_t)overflow_kw(w[1], wl[1]) : st->ov_x;
        }
        break;
    }
    case P_OVERFLOW_X:
        st->ov_x = inherit_kw || initial_kw ? OV_VISIBLE : (uint8_t)overflow_kw(value, (int)strlen(value));
        break;
    case P_OVERFLOW_Y:
        st->ov_y = inherit_kw || initial_kw ? OV_VISIBLE : (uint8_t)overflow_kw(value, (int)strlen(value));
        break;
    case P_OBJECT_FIT:
        st->obj_fit = !strcasecmp(value, "contain") ? OF_CONTAIN : !strcasecmp(value, "cover") ? OF_COVER
                    : !strcasecmp(value, "none") ? OF_NONE : !strcasecmp(value, "scale-down") ? OF_SCALE_DOWN
                    : OF_FILL;
        break;
    case P_OBJECT_POSITION:
        if (initial_kw || inherit_kw)
            st->obj_px = st->obj_py = (struct len){ 5000, 0, U_PCT };
        else
            parse_position(value, v, &st->obj_px, &st->obj_py);
        break;
    case P_BACKGROUND_SIZE: {
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "%s", value);
        st->bg_size = BGS_AUTO;
        st->bg_w.unit = st->bg_h.unit = U_AUTO;
        const char *sw[2];
        int sl[2], sn = initial_kw ? 0 : words(tmp, sw, sl, 2);
        if (!sn)
            break;
        char t0[64], t1[64] = "auto";
        snprintf(t0, sizeof(t0), "%.*s", sl[0], sw[0]);
        if (sn > 1)
            snprintf(t1, sizeof(t1), "%.*s", sl[1], sw[1]);
        if (!strcasecmp(t0, "cover"))
            st->bg_size = BGS_COVER;
        else if (!strcasecmp(t0, "contain"))
            st->bg_size = BGS_CONTAIN;
        else if (length_value(t0, v, &st->bg_w) && length_value(t1, v, &st->bg_h))
            st->bg_size = st->bg_w.unit == U_AUTO && st->bg_h.unit == U_AUTO ? BGS_AUTO : BGS_LEN;
        break;
    }
    case P_BACKGROUND_POSITION:
        st->bg_px = st->bg_py = (struct len){ 0, 0, U_PX };
        if (!initial_kw && !inherit_kw)
            parse_position(value, v, &st->bg_px, &st->bg_py);
        break;
    case P_BACKGROUND_POSITION_X:
    case P_BACKGROUND_POSITION_Y: {
        struct len l = { 0, 0, U_PX };
        char kw[64];
        snprintf(kw, sizeof(kw), "%s", value);
        if (!initial_kw && !inherit_kw) {
            /* one axis of a position: run the other axis through as 50% and take ours */
            struct len ox, oy;
            char two[96];
            snprintf(two, sizeof(two), prop == P_BACKGROUND_POSITION_X ? "%s center" : "center %s", kw);
            if (!strcasecmp(kw, "top") || !strcasecmp(kw, "bottom") || !strcasecmp(kw, "left") ||
                !strcasecmp(kw, "right"))
                snprintf(two, sizeof(two), "%s", kw);
            ox = oy = l;
            parse_position(two, v, &ox, &oy);
            l = prop == P_BACKGROUND_POSITION_X ? ox : oy;
        }
        if (prop == P_BACKGROUND_POSITION_X)
            st->bg_px = l;
        else
            st->bg_py = l;
        break;
    }
    case P_BACKGROUND_REPEAT: {
        n = initial_kw ? 0 : words(value, w, wl, 2);
        st->bg_norep_x = st->bg_norep_y = 0;
        if (n && word_is(w[0], wl[0], "repeat-x"))
            st->bg_norep_y = 1;
        else if (n && word_is(w[0], wl[0], "repeat-y"))
            st->bg_norep_x = 1;
        else if (n) {
            st->bg_norep_x = word_is(w[0], wl[0], "no-repeat");
            st->bg_norep_y = n > 1 ? word_is(w[1], wl[1], "no-repeat") : st->bg_norep_x;
        }
        break;
    }
    case P_BACKGROUND_ORIGIN:
        st->bg_origin = !strcasecmp(value, "border-box") ? 1 : !strcasecmp(value, "content-box") ? 2 : 0;
        break;
    case P_BACKGROUND_CLIP:
        st->bg_clip = !strcasecmp(value, "padding-box") ? 1 : !strcasecmp(value, "content-box") ? 2 : 0;
        break;
    case P_BACKGROUND_ATTACHMENT:
        st->bg_attachment = !strcasecmp(value, "fixed") ? 1 : !strcasecmp(value, "local") ? 2 : 0;
        break;
    case P_GRID_TEMPLATE_COLUMNS:
        if(strncasecmp(value,"subgrid",7) || subgrid_supported(value))
            st->grid_cols = !strcasecmp(value, "none") || initial_kw ? NULL : parse_tracks(value, v);
        break;
    case P_GRID_TEMPLATE_ROWS:
        if(strncasecmp(value,"subgrid",7) || subgrid_supported(value))
            st->grid_rows = !strcasecmp(value, "none") || initial_kw ? NULL : parse_tracks(value, v);
        break;
    case P_GRID_AUTO_ROWS:
        st->grid_auto_rows = !strcasecmp(value, "auto") || initial_kw ? NULL : parse_tracks(value, v);
        break;
    case P_GRID_TEMPLATE_AREAS:
        st->grid_areas = !strcasecmp(value, "none") || initial_kw ? NULL : parse_areas(value, v);
        break;
    case P_GRID_AUTO_FLOW:
        st->grid_dense = strcasestr(value, "dense") != NULL;
        st->grid_col_flow = strcasestr(value, "column") != NULL;
        break;
    case P_GRID_AUTO_COLUMNS:
        st->grid_auto_cols = !strcasecmp(value, "auto") || initial_kw ? NULL : parse_tracks(value, v);
        break;
    case P_TRANSLATE:
    case P_ROTATE:
    case P_SCALE: {
        int bit = prop == P_TRANSLATE ? 1 : prop == P_ROTATE ? 2 : 4, set;
        struct tfop op;
        if (initial_kw) {
            st->xf_ind &= (uint8_t)~bit;
        } else if (individual_transform(prop, value, v, &op, &set)) {
            st->xf_ind = (uint8_t)(set ? st->xf_ind | bit : st->xf_ind & ~bit);
            *(prop == P_TRANSLATE ? &st->xf_translate : prop == P_ROTATE ? &st->xf_rotate : &st->xf_scale) = op;
        }
        break;
    }
    case P_BACKFACE:
        st->backface_hidden = inherit_kw ? parent->backface_hidden : !strcasecmp(value, "hidden");
        break;
    case P_FILTER:
    case P_BACKDROP: {
        const struct css_filter **slot = prop == P_FILTER ? &st->filter : &st->backdrop;
        struct css_filter f;
        if (inherit_kw)
            *slot = prop == P_FILTER ? parent->filter : parent->backdrop;
        else if (initial_kw)
            *slot = NULL;
        else if (parse_filter(value, v, &f)) {
            struct css_filter *kept = f.n && v->a ? arena_alloc(v->a, sizeof(*kept)) : NULL;
            if (kept)
                *kept = f;
            *slot = kept;
        }
        break;
    }
    case P_CLIP_PATH: {
        struct css_clip cp;
        if (inherit_kw)
            st->clip_path = parent->clip_path;
        else if (initial_kw)
            st->clip_path = NULL;
        else if (parse_clip_path(value, v, &cp)) {
            struct css_clip *kept = cp.kind && v->a ? arena_alloc(v->a, sizeof(*kept)) : NULL;
            if (kept)
                *kept = cp;
            st->clip_path = kept;
        }
        break;
    }
    case P_BLEND:
        st->blend = (uint8_t)(inherit_kw ? parent->blend : initial_kw ? BL_NORMAL : blend_mode(value));
        break;
    case P_CONTAINER:                           /* container: name / type */
    case P_CONTAINER_NAME:
    case P_CONTAINER_TYPE: {
        if (inherit_kw) {
            st->cq_type = parent->cq_type;
            st->cq_name = parent->cq_name;
            break;
        }
        const char *slash = prop == P_CONTAINER ? strchr(value, '/') : NULL;
        const char *type = prop == P_CONTAINER_TYPE ? value : slash ? slash + 1 : prop == P_CONTAINER ? "normal" : NULL;
        if (prop != P_CONTAINER_TYPE) {
            int nl = (int)(slash ? slash - value : (int)strlen(value));
            while (nl > 0 && isspace((unsigned char)value[nl - 1]))
                nl--;
            st->cq_name = NULL;
            if (nl > 0 && !initial_kw && strncasecmp(value, "none", (size_t)nl) && v->a) {
                char *name = arena_alloc(v->a, (size_t)nl + 1);
                if (name) {
                    memcpy(name, value, (size_t)nl);
                    name[nl] = 0;
                    st->cq_name = name;
                }
            }
        }
        if (type) {
            while (isspace((unsigned char)*type))
                type++;
            st->cq_type = initial_kw ? CQ_NONE : !strncasecmp(type, "inline-size", 11) ? CQ_INLINE
                        : !strncasecmp(type, "size", 4) ? CQ_SIZE : CQ_NONE;
        }
        break;
    }
    case P_TEXT_WRAP:                           /* text-wrap: nowrap is white-space: nowrap; balance and pretty are hints */
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++) {
            if (word_is(w[i], wl[i], "nowrap"))
                st->white_space = st->white_space == WS_PRE_WRAP ? WS_PRE : WS_NOWRAP;
            else if (word_is(w[i], wl[i], "wrap") && (st->white_space == WS_NOWRAP || st->white_space == WS_PRE))
                st->white_space = st->white_space == WS_PRE ? WS_PRE_WRAP : WS_NORMAL;
        }
        break;
    case P_ALL: {
        /* Every property but custom ones (and direction) back to their initial or inherited
         * value. revert and revert-layer cannot bring back what a lower origin said here, so
         * they work as unset. Font size was settled in its own pass. */
        struct custom_prop *vars = st->vars;
        int fs = st->font_size;
        if (inherit_kw)
            *st = *parent;
        else if (!strcasecmp(value, "initial"))
            *st = initial_style;
        else
            inherit(st, parent);
        st->vars = vars;
        st->font_size = fs;
        memset(st->bg_values, 0, sizeof(st->bg_values));
        st->bg_layers = st->bg_next = NULL;
        break;
    }
    case P_GRID_TEMPLATE: {                     /* rows / columns, or areas with their row sizes */
        const char *vend = value + strlen(value), *slash = find_top(value, vend, '/');
        char rows[512];
        snprintf(rows, sizeof(rows), "%.*s", (int)(slash - value), value);
        st->grid_areas = strchr(rows, '"') || strchr(rows, '\'') ? parse_areas(rows, v) : NULL;
        st->grid_rows = !strcasecmp(rows, "none") || initial_kw ? NULL : parse_tracks(rows, v);
        st->grid_cols = slash >= vend || !strcasecmp(slash + 1, "none") || initial_kw
                      ? NULL : parse_tracks(slash + 1, v);
        break;
    }
    case P_GRID_COLUMN:
        grid_placement(value, &st->gc_start, &st->gc_end, &st->gc_span, parent->grid_cols);
        break;
    case P_GRID_COLUMN_START: {
        int span;
        grid_line(value, value + strlen(value), &st->gc_start, &span, parent->grid_cols);
        if (span)
            st->gc_span = span;
        break;
    }
    case P_GRID_COLUMN_END: {
        int span;
        grid_line(value, value + strlen(value), &st->gc_end, &span, parent->grid_cols);
        if (span)
            st->gc_span = span;
        break;
    }
    case P_GRID_ROW: {
        int end;
        grid_placement(value, &st->gr_start, &end, &st->gr_span, parent->grid_rows);
        st->gr_end = end;
        if (end > st->gr_start && st->gr_start > 0)
            st->gr_span = end - st->gr_start;
        break;
    }
    case P_GRID_ROW_END: {
        int span;
        grid_line(value, value + strlen(value), &st->gr_end, &span, parent->grid_rows);
        if (span)
            st->gr_span = span;
        break;
    }
    case P_GRID_ROW_START: {
        int span;
        grid_line(value, value + strlen(value), &st->gr_start, &span, parent->grid_rows);
        if (span)
            st->gr_span = span;
        break;
    }
    case P_GRID_AREA: {
        /* a name, or row-start / column-start / row-end / column-end */
        const char *e = value + strlen(value), *p = value;
        if (isalpha((unsigned char)*p) && strncasecmp(p, "span", 4) && strncasecmp(p, "auto", 4) &&
            !strchr(p, '/')) {
            st->grid_area = arena_strdup(v->a, value);
            break;
        }
        int part[4] = { 0, 0, 0, 0 }, spans[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < 4 && p <= e; i++) {
            const char *slash = find_top(p, e, '/');
            grid_line(p, slash, &part[i], &spans[i], i & 1 ? parent->grid_cols : parent->grid_rows);
            p = slash + 1;
        }
        st->gr_start = part[0], st->gc_start = part[1], st->gc_end = part[3];
        st->gr_span = spans[2] ? spans[2] : part[2] > part[0] && part[0] > 0 ? part[2] - part[0] : 0;
        st->gc_span = spans[3];
        break;
    }
    case P_BORDER_RADIUS: {
        char first[256];
        snprintf(first, sizeof(first), "%.*s", (int)strcspn(value, "/"), value);
        if (box_values(first, v, box))          /* the same 1-4 value pattern */
            memcpy(st->radius, box, sizeof(box));
        break;
    }
    case P_BORDER_TL_RADIUS: case P_BORDER_TR_RADIUS: case P_BORDER_BR_RADIUS:
    case P_BORDER_BL_RADIUS: {
        char first[64];
        set_len(&st->radius[prop - P_BORDER_TL_RADIUS], first_word(value, first, sizeof(first), NULL), v);
        break;
    }
    case P_BACKGROUND_IMAGE:
        st->gr = parse_gradient(v->a, value, v, st->color);
        st->bg_url = url_arg(v->a, value, value + strlen(value), NULL);
        break;
    case P_FLOAT:
        st->floated = !strcasecmp(value, "left") ? 1 : !strcasecmp(value, "right") ? 2 : 0;
        break;
    case P_COLOR:
        if (inherit_kw)
            st->color = parent->color;
        else if (!strcasecmp(value, "currentcolor"))
            ;
        else if (css_parse_color(value, (int)strlen(value), &c) == 0)
            st->color = ALPHA(c) ? c : st->color;
        break;
    case P_BACKGROUND_COLOR:
        if (inherit_kw)
            st->bg = parent->bg;
        else if (initial_kw)
            st->bg = TRANSPARENT;
        else if (!strcasecmp(value, "currentcolor"))
            st->bg = st->color;
        else if (css_parse_color(value, (int)strlen(value), &c) == 0)
            st->bg = c;
        break;
    case P_BACKGROUND:
        st->gr = parse_gradient(v->a, value, v, st->color);
        reset_bg_picture(st);
        apply_bg_layers(st, value, v);
        if (!strcasecmp(value, "none") || initial_kw) {
            st->bg = TRANSPARENT;
            break;
        }
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++)
            if (css_parse_color(w[i], wl[i], &c) == 0)
                st->bg = c;
        break;
    case P_FONT_SIZE: {
        int px;
        if (inherit_kw || initial_kw)
            st->font_size = inherit_kw ? parent->font_size : 16;
        else if (font_size_value(value, parent, v, &px))
            st->font_size = px;
        break;
    }
    case P_FONT_WEIGHT: {
        int b;
        if (inherit_kw)
            st->bold = parent->bold;
        else if (weight_value(value, (int)strlen(value), parent->bold, &b))
            st->bold = (uint8_t)b;
        break;
    }
    case P_FONT_STYLE:
        st->italic = !strncasecmp(value, "italic", 6) || !strncasecmp(value, "oblique", 7);
        break;
    case P_FONT_FAMILY:
        if (!inherit_kw)
            st->family = (uint8_t)family_of(value, st->family);
        break;
    case P_FONT:
        if (!inherit_kw)
            apply_font_shorthand(st, parent, value, v);
        break;
    case P_LINE_HEIGHT: {
        int num, used = parse_num(value, &num);
        struct len l;
        if (!strcasecmp(value, "normal") || initial_kw)
            st->lh_kind = LH_NORMAL;
        else if (inherit_kw)
            st->lh_kind = parent->lh_kind, st->lh = parent->lh;
        else if (used && !value[used])
            st->lh_kind = LH_FACTOR, st->lh = num;
        else if (length_value(value, v, &l) && l.unit != U_AUTO)
            st->lh_kind = LH_PX, st->lh = len_on(l, st->font_size);
        break;
    }
    case P_TEXT_ALIGN:
        st->center_blocks = !strcasecmp(value, "-webkit-center") || !strcasecmp(value, "-moz-center");
        if (inherit_kw)
            st->text_align = parent->text_align, st->center_blocks = parent->center_blocks;
        else if (!strcasecmp(value, "center") || st->center_blocks)
            st->text_align = TA_CENTER;
        else if (!strcasecmp(value, "right") || !strcasecmp(value, "end") ||
                 !strcasecmp(value, "-webkit-right"))
            st->text_align = TA_RIGHT;
        else if (!strcasecmp(value, "justify"))
            st->text_align = TA_JUSTIFY;
        else if (!strcasecmp(value, "left") || !strcasecmp(value, "start") ||
                 !strcasecmp(value, "-webkit-left") || initial_kw)
            st->text_align = TA_LEFT;
        break;
    case P_TEXT_DECORATION:
    case P_TEXT_DECORATION_LINE:
        n = words(value, w, wl, 8);
        if (n && (word_is(w[0], wl[0], "none") || word_is(w[0], wl[0], "initial")))
            st->decoration = 0;
        for (int i = 0; i < n; i++) {
            if (word_is(w[i], wl[i], "underline"))
                st->decoration |= DECO_UNDERLINE;
            else if (word_is(w[i], wl[i], "line-through"))
                st->decoration |= DECO_STRIKE;
            else if (word_is(w[i], wl[i], "overline"))
                st->decoration |= DECO_OVERLINE;
            else if (prop == P_TEXT_DECORATION && css_parse_color(w[i], wl[i], &c) == 0)
                st->deco_color = c;     /* the shorthand also takes a color and a style */
            else if (prop == P_TEXT_DECORATION && (word_is(w[i], wl[i], "double") ||
                     word_is(w[i], wl[i], "dotted") || word_is(w[i], wl[i], "dashed") ||
                     word_is(w[i], wl[i], "wavy") || word_is(w[i], wl[i], "solid")))
                st->deco_style = word_is(w[i], wl[i], "double") ? DS_DOUBLE
                               : word_is(w[i], wl[i], "dotted") ? DS_DOTTED
                               : word_is(w[i], wl[i], "dashed") ? DS_DASHED
                               : word_is(w[i], wl[i], "wavy") ? DS_WAVY : DS_SOLID;
        }
        if (prop == P_TEXT_DECORATION && n && (word_is(w[0], wl[0], "none") || word_is(w[0], wl[0], "initial")))
            st->deco_color = 0, st->deco_style = DS_SOLID;
        break;
    case P_DECO_THICKNESS:
    case P_UNDERLINE_OFFSET: {
        struct len *dst = prop == P_DECO_THICKNESS ? &st->deco_thickness : &st->underline_offset;
        struct len l;
        if (inherit_kw)
            *dst = prop == P_DECO_THICKNESS ? parent->deco_thickness : parent->underline_offset;
        else if (initial_kw || !strcasecmp(value, "auto") ||
                 (prop == P_DECO_THICKNESS && !strcasecmp(value, "from-font")))
            *dst = (struct len){ .unit = U_AUTO };
        else if (length_value(value, v, &l) && l.unit != U_AUTO &&
                 (prop != P_DECO_THICKNESS || l.v >= 0))
            *dst = l;
        break;
    }
    case P_TEXT_DECORATION_COLOR:
        if (initial_kw || !strcasecmp(value, "currentcolor"))
            st->deco_color = 0;
        else if (css_parse_color(value, (int)strlen(value), &c) == 0)
            st->deco_color = c;
        break;
    case P_TEXT_DECORATION_STYLE:
        st->deco_style = !strcasecmp(value, "double") ? DS_DOUBLE : !strcasecmp(value, "dotted") ? DS_DOTTED
                       : !strcasecmp(value, "dashed") ? DS_DASHED : !strcasecmp(value, "wavy") ? DS_WAVY
                       : DS_SOLID;
        break;
    case P_LETTER_SPACING:
    case P_WORD_SPACING: {
        struct len l;
        int px = 0;
        if (!strcasecmp(value, "normal") || initial_kw)
            px = 0;
        else if (inherit_kw)
            px = prop == P_LETTER_SPACING ? parent->letter_spacing : parent->word_spacing;
        else if (length_value(value, v, &l) && l.unit == U_PX)
            px = l.v;
        else
            break;
        if (px < -50)
            px = -50;
        if (px > 100)
            px = 100;
        *(prop == P_LETTER_SPACING ? &st->letter_spacing : &st->word_spacing) = px;
        break;
    }
    case P_WORD_BREAK:
    case P_OVERFLOW_WRAP:
        if (prop == P_WORD_BREAK && !strcasecmp(value, "break-all"))
            st->word_break = WB_BREAK_ALL;
        else if (!strcasecmp(value, "break-word") || !strcasecmp(value, "anywhere"))
            st->word_break = st->word_break == WB_BREAK_ALL ? WB_BREAK_ALL : WB_BREAK_WORD;
        else if (prop == P_WORD_BREAK && (!strcasecmp(value, "normal") || !strcasecmp(value, "keep-all")))
            st->word_break = WB_NORMAL;
        else if (prop == P_OVERFLOW_WRAP && (!strcasecmp(value, "normal") || initial_kw) &&
                 st->word_break == WB_BREAK_WORD)
            st->word_break = WB_NORMAL;
        break;
    case P_TEXT_OVERFLOW:
        st->text_ellipsis = !strncasecmp(value, "ellipsis", 8);
        break;
    case P_FONT_VARIANT:
        st->small_caps = strcasestr(value, "small-caps") != NULL || strcasestr(value, "all-small-caps") != NULL;
        if (!strcasecmp(value, "normal") || initial_kw)
            st->small_caps = 0;
        break;
    case P_TABLE_LAYOUT:
        st->table_fixed = !strcasecmp(value, "fixed");
        break;
    case P_CAPTION_SIDE:
        st->caption_bottom = !strcasecmp(value, "bottom") || !strcasecmp(value, "bottom-outside");
        break;
    case P_EMPTY_CELLS:
        st->empty_hide = !strcasecmp(value, "hide");
        break;
    case P_LIST_STYLE_POSITION:
        st->list_inside = !strcasecmp(value, "inside");
        break;
    case P_LIST_STYLE_IMAGE:
        st->ls_url = url_arg(v->a, value, value + strlen(value), NULL);
        break;
    case P_TAB_SIZE: {
        int num, used = parse_num(value, &num);
        st->tab_size = used && !value[used] && num >= 100 && num <= 1600 ? (uint8_t)(num / 100) : 8;
        break;
    }
    case P_TEXT_ALIGN_LAST:
        st->text_align_last = !strcasecmp(value, "center") ? TA_CENTER + 1
                            : !strcasecmp(value, "right") || !strcasecmp(value, "end") ? TA_RIGHT + 1
                            : !strcasecmp(value, "justify") ? TA_JUSTIFY + 1
                            : !strcasecmp(value, "left") || !strcasecmp(value, "start") ? TA_LEFT + 1 : 0;
        break;
    case P_LINE_CLAMP: {
        int num, used = parse_num(value, &num);
        st->line_clamp = used && num >= 100 && num <= 25500 ? (uint8_t)(num / 100) : 0;
        break;
    }
    case P_COUNTER_RESET:
        st->counter_reset = v->a ? arena_strdup(v->a, value) : NULL;
        break;
    case P_COUNTER_INCREMENT:
        st->counter_inc = v->a ? arena_strdup(v->a, value) : NULL;
        break;
    case P_CURSOR: {
        static const struct { const char *kw; uint8_t c; } cursors[] = {
            { "auto", CSSCUR_AUTO }, { "default", CSSCUR_DEFAULT }, { "pointer", CSSCUR_POINTER },
            { "text", CSSCUR_TEXT }, { "vertical-text", CSSCUR_TEXT }, { "wait", CSSCUR_WAIT },
            { "progress", CSSCUR_PROGRESS }, { "crosshair", CSSCUR_CROSSHAIR }, { "move", CSSCUR_MOVE },
            { "grab", CSSCUR_MOVE }, { "grabbing", CSSCUR_MOVE }, { "all-scroll", CSSCUR_MOVE },
            { "help", CSSCUR_HELP }, { "not-allowed", CSSCUR_NOT_ALLOWED }, { "no-drop", CSSCUR_NOT_ALLOWED },
            { "none", CSSCUR_NONE }, { "ns-resize", CSSCUR_RESIZE_NS }, { "n-resize", CSSCUR_RESIZE_NS },
            { "s-resize", CSSCUR_RESIZE_NS }, { "row-resize", CSSCUR_RESIZE_NS }, { "ew-resize", CSSCUR_RESIZE_EW },
            { "e-resize", CSSCUR_RESIZE_EW }, { "w-resize", CSSCUR_RESIZE_EW }, { "col-resize", CSSCUR_RESIZE_EW },
            { "nesw-resize", CSSCUR_RESIZE_ALL }, { "nwse-resize", CSSCUR_RESIZE_ALL },
            { "ne-resize", CSSCUR_RESIZE_ALL }, { "nw-resize", CSSCUR_RESIZE_ALL },
            { "se-resize", CSSCUR_RESIZE_ALL }, { "sw-resize", CSSCUR_RESIZE_ALL },
        };
        if (inherit_kw) {
            st->cursor = parent->cursor;
            break;
        }
        /* a list: url(...) fallbacks first, a keyword last; the last keyword that we know wins */
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++) {
            char kw[24];
            snprintf(kw, sizeof(kw), "%.*s", wl[i] > 20 ? 20 : wl[i], w[i]);
            char *comma = strchr(kw, ',');
            if (comma)
                *comma = 0;
            for (size_t k = 0; k < sizeof(cursors) / sizeof(cursors[0]); k++)
                if (!strcasecmp(kw, cursors[k].kw))
                    st->cursor = cursors[k].c;
        }
        if (initial_kw)
            st->cursor = CSSCUR_AUTO;
        break;
    }
    case P_POINTER_EVENTS:
        st->pointer_none = inherit_kw ? parent->pointer_none : !strcasecmp(value, "none");
        break;
    case P_USER_SELECT:
        st->select_none = inherit_kw ? parent->select_none : !strcasecmp(value, "none");
        break;
    case P_OUTLINE:
    case P_OUTLINE_STYLE:
    case P_OUTLINE_COLOR:
    case P_OUTLINE_WIDTH: {
        if (initial_kw) {
            st->outline_style = BS_NONE;
            st->outline_w = ZOOM(3);
            st->outline_color = 1;
            break;
        }
        n = words(value, w, wl, 8);
        if (prop == P_OUTLINE) {
            st->outline_style = BS_NONE;
            st->outline_w = ZOOM(3);
            st->outline_color = 1;
        }
        for (int i = 0; i < n; i++) {
            int bs = parse_border_style(w[i], wl[i]), px;
            if (word_is(w[i], wl[i], "auto"))
                bs = BS_SOLID;
            if (bs >= 0 && (prop == P_OUTLINE || prop == P_OUTLINE_STYLE))
                st->outline_style = (uint8_t)bs;
            else if ((prop == P_OUTLINE || prop == P_OUTLINE_WIDTH) && border_width_kw(w[i], wl[i], v, &px))
                st->outline_w = px;
            else if ((prop == P_OUTLINE || prop == P_OUTLINE_COLOR) && css_parse_color(w[i], wl[i], &c) == 0)
                st->outline_color = c;
            else if ((prop == P_OUTLINE || prop == P_OUTLINE_COLOR) && word_is(w[i], wl[i], "currentcolor"))
                st->outline_color = 1;
        }
        break;
    }
    case P_OUTLINE_OFFSET: {
        struct len l;
        if (length_value(value, v, &l) && l.unit == U_PX)
            st->outline_offset = l.v;
        break;
    }
    case P_TEXT_SHADOW: {
        st->ts_color = 0;
        if (!strcasecmp(value, "none") || initial_kw)
            break;
        if (inherit_kw) {
            st->ts_x = parent->ts_x, st->ts_y = parent->ts_y, st->ts_blur = parent->ts_blur;
            st->ts_color = parent->ts_color;
            break;
        }
        const char *vend = value + strlen(value), *comma = find_top(value, vend, ',');
        char first[160];
        snprintf(first, sizeof(first), "%.*s", (int)(comma - value), value);
        const char *w2[8];
        int wl2[8], nw = words(first, w2, wl2, 8), nl = 0, num[3] = { 0, 0, 0 };
        uint32_t col = 0;
        for (int i = 0; i < nw; i++) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.*s", wl2[i], w2[i]);
            struct len l;
            uint32_t cc;
            if (css_parse_color(tmp, (int)strlen(tmp), &cc) == 0 &&
                !(isdigit((unsigned char)tmp[0]) || tmp[0] == '-' || tmp[0] == '.'))
                col = cc;
            else if (length_value(tmp, v, &l) && l.unit == U_PX && nl < 3)
                num[nl++] = l.v;
        }
        if (!col)
            col = 1;                    /* currentColor, resolved when the element's style is done */
        if (nl >= 2 && (col == 1 || ALPHA(col))) {
            st->ts_x = num[0], st->ts_y = num[1], st->ts_blur = num[2] < 0 ? 0 : num[2];
            st->ts_color = col;
        }
        break;
    }
    case P_IGNORED:
        break;
    case P_TEXT_TRANSFORM:
        st->text_transform = !strcasecmp(value, "uppercase") ? TT_UPPER
                           : !strcasecmp(value, "lowercase") ? TT_LOWER
                           : !strcasecmp(value, "capitalize") ? TT_CAPITALIZE : TT_NONE;
        break;
    case P_TEXT_INDENT: {
        struct len l;
        if (length_value(value, v, &l) && l.unit == U_PX)
            st->text_indent = l.v;
        break;
    }
    case P_WHITE_SPACE:
        if (!strcasecmp(value, "pre"))
            st->white_space = WS_PRE;
        else if (!strcasecmp(value, "nowrap"))
            st->white_space = WS_NOWRAP;
        else if (!strcasecmp(value, "pre-wrap") || !strcasecmp(value, "break-spaces"))
            st->white_space = WS_PRE_WRAP;
        else if (!strcasecmp(value, "pre-line"))
            st->white_space = WS_PRE_LINE;
        else if (!strcasecmp(value, "normal") || initial_kw)
            st->white_space = WS_NORMAL;
        break;
    case P_VERTICAL_ALIGN:
        st->vertical_align = !strcasecmp(value, "sub") ? VA_SUB
                           : !strcasecmp(value, "super") ? VA_SUPER
                           : !strcasecmp(value, "middle") ? VA_MIDDLE
                           : !strcasecmp(value, "top") ? VA_TOP
                           : !strcasecmp(value, "text-top") ? VA_TOP
                           : !strcasecmp(value, "bottom") ? VA_BOTTOM
                           : !strcasecmp(value, "text-bottom") ? VA_BOTTOM : VA_BASELINE;
        break;
    case P_LIST_STYLE:
    case P_LIST_STYLE_TYPE: {
        static const struct { const char *kw; uint8_t ls; } ls[] = {
            { "disc", LS_DISC }, { "circle", LS_CIRCLE }, { "square", LS_SQUARE },
            { "decimal", LS_DECIMAL }, { "decimal-leading-zero", LS_DECIMAL },
            { "lower-alpha", LS_LOWER_ALPHA }, { "lower-latin", LS_LOWER_ALPHA },
            { "upper-alpha", LS_UPPER_ALPHA }, { "upper-latin", LS_UPPER_ALPHA },
            { "lower-roman", LS_LOWER_ROMAN }, { "upper-roman", LS_UPPER_ROMAN }, { "none", LS_NONE },
        };
        n = words(value, w, wl, 8);
        for (int i = 0; i < n; i++) {
            for (size_t k = 0; k < sizeof(ls) / sizeof(ls[0]); k++)
                if (word_is(w[i], wl[i], ls[k].kw))
                    st->list_style = ls[k].ls;
            if (prop == P_LIST_STYLE && word_is(w[i], wl[i], "inside"))
                st->list_inside = 1;
            else if (prop == P_LIST_STYLE && word_is(w[i], wl[i], "outside"))
                st->list_inside = 0;
        }
        break;
    }
    case P_MARGIN:
        if (box_values(value, v, box))
            memcpy(st->margin, box, sizeof(box));
        break;
    case P_MARGIN_TOP: case P_MARGIN_RIGHT: case P_MARGIN_BOTTOM: case P_MARGIN_LEFT:
        set_len(&st->margin[prop - P_MARGIN_TOP], value, v);
        break;
    case P_PADDING:
        if (box_values(value, v, box))
            memcpy(st->padding, box, sizeof(box));
        break;
    case P_MARGIN_INLINE: case P_MARGIN_BLOCK: case P_PADDING_INLINE: case P_PADDING_BLOCK:
    case P_INSET_INLINE: case P_INSET_BLOCK: {      /* start [end]: left and right, or top and bottom */
        const char *w[2];
        int wl[2], nw = words(value, w, wl, 2);
        char first[96], second[96];
        if (!nw || wl[0] >= (int)sizeof(first) || wl[nw - 1] >= (int)sizeof(second))
            break;
        snprintf(first, sizeof(first), "%.*s", wl[0], w[0]);
        snprintf(second, sizeof(second), "%.*s", wl[nw - 1], w[nw - 1]);
        int inl = prop == P_MARGIN_INLINE || prop == P_PADDING_INLINE || prop == P_INSET_INLINE;
        struct len *pair[2];
        if (prop == P_MARGIN_INLINE || prop == P_MARGIN_BLOCK)
            pair[0] = &st->margin[inl ? 3 : 0], pair[1] = &st->margin[inl ? 1 : 2];
        else if (prop == P_PADDING_INLINE || prop == P_PADDING_BLOCK)
            pair[0] = &st->padding[inl ? 3 : 0], pair[1] = &st->padding[inl ? 1 : 2];
        else
            pair[0] = inl ? &st->left : &st->top, pair[1] = inl ? &st->right : &st->bottom;
        set_len(pair[0], first, v);
        set_len(pair[1], second, v);
        if (prop == P_PADDING_INLINE || prop == P_PADDING_BLOCK)
            for (int i = 0; i < 2; i++)
                if (pair[i]->unit == U_AUTO)
                    *pair[i] = (struct len){ .unit = U_PX };
        break;
    }
    case P_PADDING_TOP: case P_PADDING_RIGHT: case P_PADDING_BOTTOM: case P_PADDING_LEFT:
        set_len(&st->padding[prop - P_PADDING_TOP], value, v);
        if (st->padding[prop - P_PADDING_TOP].unit == U_AUTO)
            st->padding[prop - P_PADDING_TOP] = (struct len){ .unit = U_PX };
        break;
    case P_BORDER:
        apply_border(st, value, 15, v);
        break;
    case P_BORDER_TOP: case P_BORDER_RIGHT: case P_BORDER_BOTTOM: case P_BORDER_LEFT:
        apply_border(st, value, 1 << (prop - P_BORDER_TOP), v);
        break;
    case P_BORDER_WIDTH: {
        n = words(value, w, wl, 4);
        int px[4], k = 0;
        for (int i = 0; i < n; i++)
            if (border_width_kw(w[i], wl[i], v, &px[k]))
                k++;
        if (k == 1) px[1] = px[2] = px[3] = px[0];
        else if (k == 2) px[2] = px[0], px[3] = px[1];
        else if (k == 3) px[3] = px[1];
        if (k)
            for (int i = 0; i < 4; i++)
                st->border_w[i] = px[i];
        break;
    }
    case P_BORDER_STYLE: {
        n = words(value, w, wl, 4);
        int bs[4], k = 0;
        for (int i = 0; i < n; i++)
            if ((bs[k] = parse_border_style(w[i], wl[i])) >= 0)
                k++;
        if (k == 1) bs[1] = bs[2] = bs[3] = bs[0];
        else if (k == 2) bs[2] = bs[0], bs[3] = bs[1];
        else if (k == 3) bs[3] = bs[1];
        if (k)
            for (int i = 0; i < 4; i++)
                st->border_style[i] = (uint8_t)bs[i];
        break;
    }
    case P_BORDER_COLOR: {
        n = words(value, w, wl, 4);
        uint32_t cs[4];
        int k = 0;
        for (int i = 0; i < n; i++)
            if (css_parse_color(w[i], wl[i], &cs[k]) == 0)
                k++;
        if (k == 1) cs[1] = cs[2] = cs[3] = cs[0];
        else if (k == 2) cs[2] = cs[0], cs[3] = cs[1];
        else if (k == 3) cs[3] = cs[1];
        if (k)
            for (int i = 0; i < 4; i++)
                st->border_color[i] = cs[i];
        break;
    }
    case P_BORDER_TOP_WIDTH: case P_BORDER_RIGHT_WIDTH: case P_BORDER_BOTTOM_WIDTH:
    case P_BORDER_LEFT_WIDTH: {
        int px;
        if (border_width_kw(value, (int)strlen(value), v, &px))
            st->border_w[prop - P_BORDER_TOP_WIDTH] = px;
        break;
    }
    case P_BORDER_TOP_STYLE: case P_BORDER_RIGHT_STYLE: case P_BORDER_BOTTOM_STYLE:
    case P_BORDER_LEFT_STYLE: {
        int bs = parse_border_style(value, (int)strlen(value));
        if (bs >= 0)
            st->border_style[prop - P_BORDER_TOP_STYLE] = (uint8_t)bs;
        break;
    }
    case P_BORDER_TOP_COLOR: case P_BORDER_RIGHT_COLOR: case P_BORDER_BOTTOM_COLOR:
    case P_BORDER_LEFT_COLOR:
        if (css_parse_color(value, (int)strlen(value), &c) == 0)
            st->border_color[prop - P_BORDER_TOP_COLOR] = c;
        break;
    case P_WIDTH: set_size(&st->width, &st->fit_w, value, v); break;
    case P_HEIGHT: set_size(&st->height, &st->fit_h, value, v); break;
    case P_MIN_WIDTH:
        set_len(&st->min_width, value, v);      /* auto: a flex item's own minimum */
        break;
    case P_MAX_WIDTH: set_len(&st->max_width, value, v); break;
    case P_MAX_HEIGHT: set_len(&st->max_height, value, v); break;
    case P_ASPECT_RATIO: {                      /* "16 / 9", "1.5", "auto 4/3" */
        const char *p = value;
        int a = 0, b = 100;
        st->aspect_w = st->aspect_h = 0;
        if (!strncasecmp(p, "auto", 4))
            p += 4;
        while (isspace((unsigned char)*p))
            p++;
        int used = parse_num(p, &a);
        if (used && a > 0) {
            p += used;
            while (isspace((unsigned char)*p))
                p++;
            if (*p == '/') {
                p++;
                while (isspace((unsigned char)*p))
                    p++;
                if (!parse_num(p, &b) || b <= 0)
                    b = 100;
            }
            st->aspect_w = a, st->aspect_h = b;
        }
        break;
    }
    case P_MIN_HEIGHT:
        set_len(&st->min_height, value, v);
        if (st->min_height.unit == U_AUTO)
            st->min_height = (struct len){ .unit = U_PX };
        break;
    case P_BOX_SIZING:
        st->box_sizing_border = !strcasecmp(value, "border-box");
        break;
    case P_VISIBILITY:
        if (!inherit_kw)
            st->hidden = !strcasecmp(value, "hidden") || !strcasecmp(value, "collapse");
        break;
    case P_BORDER_SPACING: {
        struct len l;
        if (length_value(value, v, &l) && l.unit == U_PX)
            st->border_spacing = l.v;
        break;
    }
    case P_BORDER_COLLAPSE:
        if (!strcasecmp(value, "collapse"))
            st->border_spacing = 0;
        break;
    case P_ANCHOR_NAME:
        st->anchor_name = initial_kw || !strcasecmp(value, "none") || !v->a ? NULL : arena_strdup(v->a, value);
        break;
    case P_POSITION_ANCHOR:
        st->position_anchor = initial_kw || !strcasecmp(value, "auto") || !v->a ? NULL : arena_strdup(v->a, value);
        break;
    case P_POSITION_AREA: {
        uint8_t area[4] = { 0 };
        if (!initial_kw && !parse_area(value, area))
            break;
        struct anchor_spec *sp = area[1] || (st->anchor && st->anchor->area[1]) ? anchor_spec_edit(st, v->a) : NULL;
        if (sp)
            memcpy(sp->area, area, 4);
        break;
    }
    case P_POSITION_TRY: case P_POSITION_TRY_FALLBACKS: {
        struct anchor_spec tries;
        const char *list = value;
        if (prop == P_POSITION_TRY) {           /* position-try: [order] fallbacks */
            static const char *const orders[] = { "normal", "most-width", "most-height", "most-block-size",
                                                  "most-inline-size", NULL };
            for (int i = 0; orders[i]; i++)
                if (!strncasecmp(list, orders[i], strlen(orders[i])) && isspace((unsigned char)list[strlen(orders[i])]))
                    list += strlen(orders[i]);
        }
        parse_tries(initial_kw ? "none" : list, &tries);
        struct anchor_spec *sp = tries.ntry || (st->anchor && st->anchor->ntry) ? anchor_spec_edit(st, v->a) : NULL;
        if (sp) {
            sp->ntry = tries.ntry;
            memcpy(sp->tries, tries.tries, sizeof(sp->tries));
        }
        break;
    }
    case P_POSITION:
        st->positioned = !strcasecmp(value, "absolute") || !strcasecmp(value, "fixed");
        st->position = !strcasecmp(value, "relative") ? POS_RELATIVE : !strcasecmp(value, "absolute") ? POS_ABSOLUTE
                     : !strcasecmp(value, "fixed") ? POS_FIXED
                     : !strcasecmp(value, "sticky") || !strcasecmp(value, "-webkit-sticky") ? POS_STICKY
                     : POS_STATIC;
        break;
    case P_BOX_SHADOW: {
        st->sh_color = 0;
        const char *vend = value + strlen(value), *comma = find_top(value, vend, ',');
        char first[160];
        snprintf(first, sizeof(first), "%.*s", (int)(comma - value), value);
        if (!strcasecmp(first, "none") || strcasestr(first, "inset"))
            break;
        const char *w2[8];
        int wl2[8], nw = words(first, w2, wl2, 8), nl = 0, num[4] = { 0, 0, 0, 0 };
        uint32_t col = OPAQUE(0x000000);
        for (int i = 0; i < nw; i++) {
            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%.*s", wl2[i], w2[i]);
            struct len l;
            uint32_t cc;
            if (css_parse_color(tmp, (int)strlen(tmp), &cc) == 0 && !(isdigit((unsigned char)tmp[0]) || tmp[0] == '-' || tmp[0] == '.'))
                col = cc;
            else if (length_value(tmp, v, &l) && l.unit == U_PX && nl < 4)
                num[nl++] = l.v;
        }
        if (nl >= 2 && ALPHA(col)) {
            st->sh_x = num[0], st->sh_y = num[1], st->sh_blur = num[2] < 0 ? 0 : num[2], st->sh_spread = num[3];
            st->sh_color = col;
        }
        break;
    }
    case P_TRANSFORM: {
        struct css_xform parsed;
        if (parse_transform(value,v,&parsed)) {
            st->xf=NULL;st->tx=st->ty=(struct len){0};
            if(parsed.n && v->a){struct css_xform *xf=arena_alloc(v->a,sizeof(*xf));if(xf){*xf=parsed;st->xf=xf;}}
        }
        break;
    }
    case P_PERSPECTIVE: {
        struct len l;
        if (!strcasecmp(value,"none") || initial_kw)st->perspective=(struct len){.unit=U_AUTO};
        else if(length_value(value,v,&l) && l.unit==U_PX && l.v>=0)st->perspective=l;
        break;
    }
    case P_PERSPECTIVE_ORIGIN:parse_position(value,v,&st->pox,&st->poy);break;
    case P_TRANSFORM_ORIGIN: {
        const char *parts[4];int lengths[4],count=words(value,parts,lengths,4);
        if(count==3) {
            char xy[256];snprintf(xy,sizeof(xy),"%.*s %.*s",lengths[0],parts[0],lengths[1],parts[1]);
            struct len z;char zs[64];snprintf(zs,sizeof(zs),"%.*s",lengths[2],parts[2]);
            if(length_value(zs,v,&z) && z.unit==U_PX) {
                parse_position(xy,v,&st->tox,&st->toy);st->toz=z;
            }
        } else if(count<=2)parse_position(value,v,&st->tox,&st->toy);
        break;
    }
    case P_Z_INDEX: {
        int num;
        st->z_set = parse_num(value, &num) != 0;
        st->z_index = st->z_set ? num / 100 : 0;
        break;
    }
    case P_CLEAR:
        st->clear = !strcasecmp(value, "both") ? 3 : !strcasecmp(value, "left") ? 1 : !strcasecmp(value, "right") ? 2 : 0;
        break;
    case P_RIGHT: set_len(&st->right, value, v); break;
    case P_BOTTOM: set_len(&st->bottom, value, v); break;
    case P_INSET: {                             /* top, right, bottom, left */
        struct len ib[4];
        if (box_values(value, v, ib)) {
            st->top = ib[0], st->right = ib[1], st->bottom = ib[2], st->left = ib[3];
            if (ib[0].unit == U_PX && ib[3].unit == U_PX && (ib[0].v <= -500 || ib[3].v <= -500))
                st->invisible |= 2;
        }
        break;
    }
    case P_OPACITY: {
        int num, used = parse_num(value, &num);
        if (used) {
            if (value[used] == '%')
                num /= 100;
            num = num < 0 ? 0 : num > 100 ? 100 : num;
            st->opacity = (uint8_t)((num * 255 + 50) / 100);
            st->invisible = (uint8_t)((st->invisible & ~1) | (num <= 0));
        } else if (initial_kw || inherit_kw) {
            st->opacity = inherit_kw ? parent->opacity : 255;
        }
        break;
    }
    case P_TRANSITION: case P_TRANSITION_PROPERTY: case P_TRANSITION_DURATION: case P_TRANSITION_TIMING:
    case P_TRANSITION_DELAY: case P_TRANSITION_BEHAVIOR: case P_ANIMATION: case P_ANIMATION_NAME: case P_ANIMATION_DURATION:
    case P_ANIMATION_TIMING: case P_ANIMATION_DELAY: case P_ANIMATION_ITERATION: case P_ANIMATION_DIRECTION:
    case P_ANIMATION_FILL: case P_ANIMATION_STATE:
        apply_motion(st, parent, prop, value, inherit_kw, initial_kw, v);
        break;
    case P_LEFT:
    case P_TOP: {
        /* Only to recognise content pushed off the screen to hide it. */
        struct len l;
        if (length_value(value, v, &l)) {
            if (prop == P_LEFT)
                st->left = l;
            else
                st->top = l;
        }
        if (length_value(value, v, &l) && l.unit == U_PX)
            st->invisible = (uint8_t)((st->invisible & ~2) | (l.v <= -500 ? 2 : 0));
        break;
    }
    case P_MASK:
        /* An icon painted through a mask image: without the mask it would be a box. */
        if (strstr(value, "url("))
            st->invisible |= 8;
        break;
    case P_CLIP:
        if (!strncasecmp(value, "rect(", 5)) {
            int num, zero = 1;
            for (const char *p = value + 5; *p && *p != ')'; p++)
                if (isdigit((unsigned char)*p) && parse_num(p, &num) && num > 100)
                    zero = 0;
            st->invisible = (uint8_t)((st->invisible & ~4) | (zero ? 4 : 0));
        }
        break;
    }
}

static int background_parts(const char *value)
{
    int n = 1;
    if (!value) return 0;
    const char *end = value + strlen(value);
    for (const char *p = value; p < end;) {
        p = find_top(p, end, ',');
        if (p < end) n++, p++;
    }
    return n;
}

static const char *background_part(struct arena *a, const char *value, int i)
{
    int count = background_parts(value);
    if (!count) return NULL;
    i %= count;
    const char *p = value, *end = p + strlen(p);
    while (i--) p = find_top(p, end, ',') + 1;
    return trimmed(a, p, find_top(p, end, ','));
}

/* Image layers determine the length. Other lists repeat, as CSS requires. */
static void finish_background(struct style *st, const struct style *parent, const struct vctx *v)
{
    const char **raw = st->bg_values;
    if (!raw[0] && !raw[1]) return;
    int n = background_parts(raw[1] ? raw[1] : raw[0]);
    if (n > 64) n = 64;
    struct style **tail = &st->bg_layers;
    static const int props[] = { P_BACKGROUND_SIZE, P_BACKGROUND_POSITION, P_BACKGROUND_REPEAT,
        P_BACKGROUND_ORIGIN, P_BACKGROUND_CLIP, P_BACKGROUND_ATTACHMENT,
        P_BACKGROUND_POSITION_X, P_BACKGROUND_POSITION_Y };
    for (int i = 0; i < n; i++) {
        struct style *l = arena_alloc(v->a, sizeof(*l));
        if (!l) break;
        *l = *st;
        l->bg_layers = l->bg_next = NULL;
        reset_bg_picture(l);
        l->bg_clip = l->bg_attachment = 0;
        l->gr = NULL;
        const char *part = background_part(v->a, raw[0], i);
        if (part) {
            l->bg_url = url_arg(v->a, part, part + strlen(part), NULL);
            l->gr = parse_gradient(v->a, part, v, st->color);
            parse_bg_layer(l, part, part + strlen(part), v);
        }
        if (raw[1]) {
            part = background_part(v->a, raw[1], i);
            l->bg_url = url_arg(v->a, part, part + strlen(part), NULL);
            l->gr = parse_gradient(v->a, part, v, st->color);
        }
        for (int k = 2; k < 10; k++)
            if (raw[k]) apply(l, parent, props[k - 2], background_part(v->a, raw[k], i), v);
        *tail = l;
        tail = &l->bg_next;
        if (i == n - 1) st->bg_clip = l->bg_clip;
    }
}

/* ---- var() ---- */

static const char *lookup_var(const struct style *st, const char *name, int len)
{
    for (const struct custom_prop *c = st->vars; c; c = c->next)
        if ((int)strlen(c->name) == len && !strncmp(c->name, name, (size_t)len))
            return c->value;
    return NULL;
}

/* Does value hold something substitute() puts in: var(), env() or image-set()? */
static int substituted(const char *value)
{
    return strstr(value, "var(") || strstr(value, "env(") || strstr(value, "image-set(");
}

/* env(name, fallback): this screen has no notch, keyboard or title bar areas. */
static int env_value(const char *args, const char *end, char *out, int size)
{
    while (args < end && isspace((unsigned char)*args))
        args++;
    const char *comma = find_top(args, end, ','), *ne = comma;
    while (ne > args && isspace((unsigned char)ne[-1]))
        ne--;
    int nl = (int)(ne - args);
    int known = (nl > 15 && !strncmp(args, "safe-area-inset", 15)) ||
                (nl > 19 && !strncmp(args, "safe-area-max-inset", 19)) ||
                (nl > 14 && !strncmp(args, "keyboard-inset", 14)) || (nl > 14 && !strncmp(args, "titlebar-area-", 14));
    if (!known && comma < end) {
        int fl = (int)(end - comma - 1);
        if (fl >= size)
            return 0;
        memcpy(out, comma + 1, (size_t)fl);
        out[fl] = 0;
        return 1;
    }
    snprintf(out, (size_t)size, "0px");
    return 1;
}

/* image-set("a.png" 1x, url(b.png) 2x, ...): the one for this screen, as url(); types we cannot
 * decode are passed over. */
static int image_set_value(const char *args, const char *end, char *out, int size)
{
    const char *best = NULL, *best_end = NULL;
    int best_one_x = 0;
    for (const char *p = args; p < end;) {
        const char *comma = find_top(p, end, ','), *c = p;
        while (c < comma && isspace((unsigned char)*c))
            c++;
        const char *ce = c;
        if (ce < comma && (*ce == '"' || *ce == '\'')) {
            char q = *ce++;
            while (ce < comma && *ce != q)
                ce += *ce == '\\' ? 2 : 1;
            if (ce < comma)
                ce++;
        } else {
            while (ce < comma && !isspace((unsigned char)*ce))
                ce = *ce == '(' ? skip_group(ce, comma) : ce + 1;
        }
        /* what follows the image: its resolution and perhaps type("image/avif") */
        char rest[128];
        int rl = (int)(comma - ce) < 127 ? (int)(comma - ce) : 127;
        for (int i = 0; i < rl; i++)
            rest[i] = (char)tolower((unsigned char)ce[i]);
        rest[rl] = 0;
        const char *type = strstr(rest, "type(");
        int decodable = !type || strstr(type, "png") || strstr(type, "jpeg") || strstr(type, "gif") ||
                        strstr(type, "webp") || strstr(type, "svg");
        const char *digit = strpbrk(rest, "0123456789.");
        int one_x = !digit || strtod(digit, NULL) <= 1.0;
        if (ce > c && decodable && (!best || (one_x && !best_one_x))) {
            best = c, best_end = ce, best_one_x = one_x;
            if (one_x)
                break;
        }
        p = comma < end ? comma + 1 : end;
    }
    if (!best)
        return 0;
    int n = (int)(best_end - best);
    if (*best == '"' || *best == '\'')
        return snprintf(out, (size_t)size, "url(%.*s)", n, best) < size;
    return snprintf(out, (size_t)size, "%.*s", n, best) < size;
}

/* Expand var(--x, fallback), env() and image-set() in value; returns 0 if a variable is missing. */
static int substitute(const struct style *st, const char *value, char *out, int size, int depth)
{
    static int expansions;              /* in this whole value: nested ones can multiply */
    if (!depth)
        expansions = 0;
    int o = 0;
    const char *end = value + strlen(value);
    for (const char *p = value; p < end;) {
        int env = !strncmp(p, "env(", 4), set = !strncmp(p, "image-set(", 10) || !strncmp(p, "-webkit-image-set(", 18);
        if ((env || set) && (p == value || !is_ident_char(p[-1]) || p[-1] == '-')) {
            const char *open = strchr(p, '('), *close = skip_group(open, end);
            char inner[1024], done[1024];
            if (++expansions > 1024)
                return 0;
            if (!(env ? env_value(open + 1, close - 1, inner, sizeof(inner))
                      : image_set_value(open + 1, close - 1, inner, sizeof(inner))) ||
                depth > 8 || !substitute(st, inner, done, sizeof(done), depth + 1))
                return 0;
            int il = (int)strlen(done);
            if (o + il >= size)
                return 0;
            memcpy(out + o, done, (size_t)il);
            o += il;
            p = close;
            continue;
        }
        if (!strncmp(p, "var(", 4)) {
            const char *close = skip_group(p + 3, end);
            const char *args = p + 4, *args_end = close - 1;
            while (args < args_end && isspace((unsigned char)*args))
                args++;
            const char *comma = find_top(args, args_end, ',');
            const char *ne = comma;
            while (ne > args && isspace((unsigned char)ne[-1]))
                ne--;
            const char *val = lookup_var(st, args, (int)(ne - args));
            char fallback[512];
            if (!val && comma < args_end) {
                int fl = (int)(args_end - comma - 1);
                if (fl >= (int)sizeof(fallback))
                    return 0;
                memcpy(fallback, comma + 1, (size_t)fl);
                fallback[fl] = 0;
                val = fallback;
            }
            if (!val || depth > 8 || ++expansions > 1024)
                return 0;
            char inner[1024];
            if (!substitute(st, val, inner, sizeof(inner), depth + 1))
                return 0;
            int il = (int)strlen(inner);
            if (o + il >= size)
                return 0;
            memcpy(out + o, inner, (size_t)il);
            o += il;
            p = close;
            continue;
        }
        if (o + 1 >= size)
            return 0;
        out[o++] = *p++;
    }
    out[o] = 0;
    /* Trim what the substitution left around the value. */
    char *s = out;
    while (isspace((unsigned char)*s))
        s++;
    if (s != out)
        memmove(out, s, strlen(s) + 1);
    int l = (int)strlen(out);
    while (l > 0 && isspace((unsigned char)out[l - 1]))
        out[--l] = 0;
    return 1;
}

/* ---- presentational HTML attributes ---- */

static int legacy_color(const char *v, char *out, int size)
{
    uint32_t c;
    if (css_parse_color(v, (int)strlen(v), &c) == 0) {
        snprintf(out, (size_t)size, "%s", v);
        return 1;
    }
    /* "ff0000" without the '#' */
    int n = 0;
    while (v[n] && isxdigit((unsigned char)v[n]))
        n++;
    if ((n == 6 || n == 3) && !v[n]) {
        snprintf(out, (size_t)size, "#%s", v);
        return 1;
    }
    return 0;
}

static void hint_len(char *buf, int size, const char *prop, const char *v)
{
    int n = (int)strlen(buf);
    if (!v || !isdigit((unsigned char)*v))
        return;
    int num = atoi(v);
    snprintf(buf + n, (size_t)(size - n), "%s: %d%s;", prop, num, strchr(v, '%') ? "%" : "px");
}

static void hint_color(char *buf, int size, const char *prop, const char *v)
{
    char c[64];
    int n = (int)strlen(buf);
    if (v && legacy_color(v, c, sizeof(c)))
        snprintf(buf + n, (size_t)(size - n), "%s: %s;", prop, c);
}

static void hint_add(char *buf, int size, const char *text)
{
    int n = (int)strlen(buf);
    snprintf(buf + n, (size_t)(size - n), "%s", text);
}

/* The old background="picture.png" attribute. */
static void hint_background(char *buf, int size, const char *url)
{
    if (!url || !*url || strpbrk(url, "\"\\\n"))
        return;
    int n = (int)strlen(buf);
    snprintf(buf + n, (size_t)(size - n), "background-image: url(\"%.400s\");", url);
}

static const struct node *ancestor(const struct node *n, int tag)
{
    for (n = n->parent; n; n = n->parent)
        if (n->tag == tag)
            return n;
    return NULL;
}

static void hint_align(char *buf, int size, const char *v, int block)
{
    if (!v)
        return;
    if (!strcasecmp(v, "center") || !strcasecmp(v, "middle"))
        hint_add(buf, size, block ? "margin-left: auto; margin-right: auto;" : "text-align: -webkit-center;");
    else if (!strcasecmp(v, "right"))
        hint_add(buf, size, block ? "margin-left: auto;" : "text-align: right;");
    else if (!strcasecmp(v, "left") && !block)
        hint_add(buf, size, "text-align: left;");
    else if (!strcasecmp(v, "justify") && !block)
        hint_add(buf, size, "text-align: justify;");
}

/* The declarations HTML attributes ask for, as CSS text. */
static void hints(const struct node *n, char *buf, int size)
{
    const char *v;
    buf[0] = 0;
    switch (n->tag) {
    case T_BODY:
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_background(buf, size, node_attr(n, "background"));
        hint_color(buf, size, "color", node_attr(n, "text"));
        hint_len(buf, size, "margin", node_attr(n, "marginwidth"));
        if ((v = node_attr(n, "leftmargin")) || (v = node_attr(n, "topmargin")))
            hint_len(buf, size, "margin", v);
        break;
    case T_FONT:
        hint_color(buf, size, "color", node_attr(n, "color"));
        if ((v = node_attr(n, "size"))) {
            static const char *const sizes[] = { "x-small", "x-small", "small", "medium", "large",
                                                 "x-large", "xx-large", "xxx-large" };
            int k = atoi(v);
            if (*v == '+' || *v == '-')
                k += 3;
            k = k < 1 ? 1 : k > 7 ? 7 : k;
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "font-size: %s;", sizes[k]);
        }
        if ((v = node_attr(n, "face"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "font-family: %.100s;", v);
        }
        break;
    case T_TABLE:
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_background(buf, size, node_attr(n, "background"));
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        hint_len(buf, size, "border-spacing", node_attr(n, "cellspacing"));
        hint_align(buf, size, node_attr(n, "align"), 1);
        if ((v = node_attr(n, "border")) && (atoi(v) > 0 || !*v)) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "border: %dpx outset #808080;", *v ? atoi(v) : 1);
        }
        break;
    case T_TR:
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_align(buf, size, node_attr(n, "align"), 0);
        break;
    case T_TD:
    case T_TH: {
        hint_color(buf, size, "background-color", node_attr(n, "bgcolor"));
        hint_background(buf, size, node_attr(n, "background"));
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        hint_align(buf, size, node_attr(n, "align"), 0);
        const struct node *tr = n->parent && n->parent->tag == T_TR ? n->parent : NULL;
        if (!node_attr(n, "align") && tr)
            hint_align(buf, size, node_attr(tr, "align"), 0);
        if (!(v = node_attr(n, "valign")) && tr)
            v = node_attr(tr, "valign");
        if (v) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "vertical-align: %s;",
                     !strcasecmp(v, "top") ? "top" : !strcasecmp(v, "bottom") ? "bottom" : "middle");
        }
        if (node_attr(n, "nowrap"))
            hint_add(buf, size, "white-space: nowrap;");
        const struct node *table = ancestor(n, T_TABLE);
        if (table) {
            hint_len(buf, size, "padding", node_attr(table, "cellpadding"));
            if ((v = node_attr(table, "border")) && (atoi(v) > 0 || !*v))
                hint_add(buf, size, "border: 1px inset #808080;");
        }
        break;
    }
    case T_IFRAME:
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        if ((v = node_attr(n, "frameborder")) && (!strcmp(v, "0") || !strcasecmp(v, "no")))
            hint_add(buf, size, "border: none;");
        break;
    case T_SVG:
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        break;
    case T_IMG:
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_len(buf, size, "height", node_attr(n, "height"));
        if ((v = node_attr(n, "border"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "border: %dpx solid;", atoi(v));
        }
        if ((v = node_attr(n, "hspace"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "margin-left: %dpx; margin-right: %dpx;", atoi(v), atoi(v));
        }
        if ((v = node_attr(n, "vspace"))) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "margin-top: %dpx; margin-bottom: %dpx;", atoi(v), atoi(v));
        }
        if ((v = node_attr(n, "align"))) {
            if (!strcasecmp(v, "middle") || !strcasecmp(v, "absmiddle") || !strcasecmp(v, "center"))
                hint_add(buf, size, "vertical-align: middle;");
            else if (!strcasecmp(v, "left") || !strcasecmp(v, "right"))
                hint_add(buf, size, "float: left;");
            else if (!strcasecmp(v, "top") || !strcasecmp(v, "texttop"))
                hint_add(buf, size, "vertical-align: top;");
        }
        break;
    case T_HR:
        hint_len(buf, size, "width", node_attr(n, "width"));
        hint_align(buf, size, node_attr(n, "align"), 1);
        if ((v = node_attr(n, "size")) && atoi(v) > 1) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "height: %dpx;", atoi(v) - 2);
        }
        if ((v = node_attr(n, "color"))) {
            char c[64];
            if (legacy_color(v, c, sizeof(c))) {
                int l = (int)strlen(buf);
                snprintf(buf + l, (size_t)(size - l), "border: 1px solid %s; background-color: %s;", c, c);
            }
        } else if (node_attr(n, "noshade")) {
            hint_add(buf, size, "border: 1px solid #808080; background-color: #808080;");
        }
        break;
    case T_DIV: case T_P: case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
    case T_CAPTION: case T_THEAD: case T_TBODY: case T_TFOOT: case T_LEGEND:
        hint_align(buf, size, node_attr(n, "align"), 0);
        break;
    case T_UL: case T_OL: case T_LI:
        if ((v = node_attr(n, "type"))) {
            const char *t = !strcmp(v, "1") ? "decimal" : !strcmp(v, "a") ? "lower-alpha"
                          : !strcmp(v, "A") ? "upper-alpha" : !strcmp(v, "i") ? "lower-roman"
                          : !strcmp(v, "I") ? "upper-roman" : !strcasecmp(v, "circle") ? "circle"
                          : !strcasecmp(v, "square") ? "square" : !strcasecmp(v, "disc") ? "disc" : NULL;
            if (t) {
                int l = (int)strlen(buf);
                snprintf(buf + l, (size_t)(size - l), "list-style-type: %s;", t);
            }
        }
        break;
    case T_INPUT:
        if ((v = node_attr(n, "size")) && atoi(v) > 0) {
            int l = (int)strlen(buf);
            snprintf(buf + l, (size_t)(size - l), "width: %dpx;", atoi(v) * 7 + 8);
        }
        break;
    case T_TEXTAREA: {
        int cols = (v = node_attr(n, "cols")) ? atoi(v) : 20;
        int rows = (v = node_attr(n, "rows")) ? atoi(v) : 2;
        cols = cols < 1 ? 20 : cols > 200 ? 200 : cols;
        rows = rows < 1 ? 2 : rows > 100 ? 100 : rows;
        int l = (int)strlen(buf);
        snprintf(buf + l, (size_t)(size - l), "width: %dpx; height: %dpx;", cols * 8 + 8, rows * 16 + 6);
        break;
    }
    case T_PRE:
        if (node_attr(n, "wrap"))
            hint_add(buf, size, "white-space: pre-wrap;");
        break;
    default:
        break;
    }
}

/* ---- the cascade ---- */

struct match {
    const struct decl *d;
    uint32_t rank;              /* origin and importance */
    uint32_t spec;
    uint32_t order;
    uint32_t pe;                /* for ::before (1) or ::after (2) */
    int layer;
    int scope;                 /* inner tree normal rules precede outer rules; important reverses */
};

enum { R_UA, R_HINT, R_AUTHOR, R_INLINE, R_AUTHOR_IMPORTANT, R_INLINE_IMPORTANT };

static int cmp_match(const void *a, const void *b)
{
    const struct match *x = a, *y = b;
    if (x->rank != y->rank && (x->rank < R_AUTHOR || y->rank < R_AUTHOR ||
                              x->d->important != y->d->important))
        return x->rank < y->rank ? -1 : 1;
    if (x->scope != y->scope)
        return x->d->important ? (x->scope < y->scope ? 1 : -1)
                               : (x->scope < y->scope ? -1 : 1);
    if (x->rank != y->rank)
        return x->rank < y->rank ? -1 : 1;
    if (x->layer != y->layer)
        return x->d->important ? (x->layer < y->layer ? 1 : -1)
                               : (x->layer < y->layer ? -1 : 1);
    if (x->spec != y->spec)
        return x->spec < y->spec ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

/* Origins, lowest first. Important author declarations beat everything. */


struct collect {
    struct match *m;
    int n;
    const struct mctx *mc;
    int scope;
};

static void add_decls(struct collect *c, const struct decl *d, int nd, int base_rank,
                      uint32_t spec, uint32_t order, int pe, int layer)
{
    for (int i = 0; i < nd && c->n < MAX_MATCHES; i++) {
        uint32_t rank = (uint32_t)base_rank;
        if (d[i].important && base_rank == R_AUTHOR)
            rank = R_AUTHOR_IMPORTANT;
        else if (d[i].important && base_rank == R_INLINE)
            rank = R_INLINE_IMPORTANT;
        c->m[c->n++] = (struct match){ &d[i], rank, spec, order + (uint32_t)i, (uint32_t)pe, layer, c->scope };
    }
}

static void match_list(struct collect *c, const struct sheet *sh, const struct ref *ref,
                       const struct node *n, int rank, uint32_t sheet_order)
{
    for (; ref; ref = ref->next) {
        const struct rule *r = &sh->rules[ref->rule];
        const struct selector *sel = &r->sels[ref->sel];
        if (r->starting && !cur_starting)
            continue;
        if (cur_bloom && !cur_bloom->broken && sel->nanc) {   /* an ancestor it needs is not there */
            int i = 0;
            while (i < sel->nanc && bloom_has(cur_bloom, sel->anc[i]))
                i++;
            if (i < sel->nanc)
                continue;
        }
        if (match_from(sel, sel->nparts - 1, n, c->mc) && (!r->cq || cq_matches(r->cq, (struct node *)n, sel->pe))) {
            add_decls(c, r->decls, r->ndecls, rank, sel->spec,
                      sheet_order + (uint32_t)ref->rule * 256, sel->pe,
                      r->layer ? r->layer->canonical->order : 0x7fffffff);
            starting_hits += r->starting;
        }
    }
}

static void match_sheet(struct collect *c, const struct sheet *sh, const struct node *n, int rank,
                        uint32_t sheet_order)
{
    if (c->mc->host && (n == c->mc->host || n == c->mc->slotted)) {
        for (int i = 0; i < sh->nrules; i++) {
            const struct rule *r = &sh->rules[i];
            if (r->starting && !cur_starting)
                continue;
            for (int j = 0; j < r->nsels; j++) {
                const struct selector *sel = &r->sels[j];
                if (n == c->mc->slotted && !sel->parts[sel->nparts-1].nslot) continue;
                if (match_from(sel, sel->nparts-1, n, c->mc) && (!r->cq || cq_matches(r->cq, (struct node *)n, sel->pe))) {
                    add_decls(c,r->decls,r->ndecls,rank,sel->spec,sheet_order+(uint32_t)i*256,sel->pe,
                              r->layer ? r->layer->canonical->order : 0x7fffffff);
                    starting_hits += r->starting;
                }
            }
        }
        return;
    }
    const struct nkeys *k = node_keys(n);
    if (k) {
        if (k->id)
            match_list(c, sh, sh->by_id[k->id_h % INDEX_BUCKETS], n, rank, sheet_order);
        /* Each class's bucket once, even if two classes share it (however many classes). */
        uint32_t seen[INDEX_BUCKETS / 32] = { 0 };
        for (int i = 0; i < k->ncls; i++) {
            uint32_t b = k->cls[i].h % INDEX_BUCKETS;
            if (seen[b / 32] & (1u << (b % 32)))
                continue;
            seen[b / 32] |= 1u << (b % 32);
            match_list(c, sh, sh->by_class[b], n, rank, sheet_order);
        }
        if (n->tag > 0)
            match_list(c, sh, sh->by_tag[n->tag], n, rank, sheet_order);
        match_list(c, sh, sh->other, n, rank, sheet_order);
        return;
    }
    const char *id = node_attr(n, "id");
    if (id)
        match_list(c, sh, sh->by_id[hash_str(id) % INDEX_BUCKETS], n, rank, sheet_order);
    const char *cls = node_attr(n, "class");
    if (cls) {
        /* Each class's bucket once, even if two classes share it. */
        uint32_t seen[16];
        int nseen = 0;
        for (const char *p = cls; *p;) {
            while (*p && isspace((unsigned char)*p))
                p++;
            const char *e = p;
            while (*e && !isspace((unsigned char)*e))
                e++;
            if (e > p) {
                char one[128];
                int l = (int)(e - p) < 127 ? (int)(e - p) : 127;
                memcpy(one, p, (size_t)l);
                one[l] = 0;
                uint32_t b = hash_str(one) % INDEX_BUCKETS;
                int dup = 0;
                for (int i = 0; i < nseen; i++)
                    dup |= seen[i] == b;
                if (!dup && nseen < 16) {
                    seen[nseen++] = b;
                    match_list(c, sh, sh->by_class[b], n, rank, sheet_order);
                }
            }
            p = e;
        }
    }
    if (n->tag > 0)
        match_list(c, sh, sh->by_tag[n->tag], n, rank, sheet_order);
    match_list(c, sh, sh->other, n, rank, sheet_order);
}

/* Copy only the property being rolled back. In particular, reverting a
 * longhand must preserve the other values set by a higher-layer shorthand. */
static void restore_property(struct style *st, const struct style *src, int prop)
{
#define COPY(f) memcpy(&st->f, &src->f, sizeof(st->f))
    switch (prop) {
    case P_DISPLAY: COPY(display); COPY(inner); break;
    case P_FLEX_DIRECTION: COPY(flex_column); COPY(flex_reverse); break;
    case P_FLEX_FLOW: COPY(flex_column); COPY(flex_reverse); COPY(flex_wrap); break;
    case P_FLEX_WRAP: COPY(flex_wrap); break;
    case P_FLEX: COPY(flex_grow); COPY(flex_shrink); COPY(flex_basis); break;
    case P_FLEX_GROW: COPY(flex_grow); break;
    case P_FLEX_SHRINK: COPY(flex_shrink); break;
    case P_FLEX_BASIS: COPY(flex_basis); break;
    case P_JUSTIFY_CONTENT: COPY(justify); break;
    case P_ALIGN_ITEMS: COPY(align_items); break;
    case P_ALIGN_CONTENT: COPY(align_content); break;
    case P_GRID_AUTO_COLUMNS: COPY(grid_auto_cols); break;
    case P_TRANSLATE: COPY(xf_translate); COPY(xf_ind); break;
    case P_ROTATE: COPY(xf_rotate); COPY(xf_ind); break;
    case P_SCALE: COPY(xf_scale); COPY(xf_ind); break;
    case P_TEXT_WRAP: COPY(white_space); break;
    case P_ALIGN_SELF: COPY(align_self); break;
    case P_JUSTIFY_ITEMS: COPY(justify_items); break;
    case P_JUSTIFY_SELF: COPY(justify_self); break;
    case P_PLACE_CONTENT: COPY(justify); COPY(align_content); break;
    case P_PLACE_ITEMS: COPY(align_items); COPY(justify_items); break;
    case P_PLACE_SELF: COPY(align_self); COPY(justify_self); break;
    case P_ORDER: COPY(order); break;
    case P_GAP: COPY(row_gap_normal); COPY(row_gap); COPY(col_gap); COPY(col_gap_normal); break;
    case P_ROW_GAP: COPY(row_gap_normal); COPY(row_gap); break;
    case P_COLUMN_GAP: COPY(col_gap); COPY(col_gap_normal); break;
    case P_COLUMNS: COPY(col_count); COPY(col_width); break;
    case P_COLUMN_COUNT: COPY(col_count); break;
    case P_COLUMN_WIDTH: COPY(col_width); break;
    case P_COLUMN_RULE: COPY(col_rule_style); COPY(col_rule_w); COPY(col_rule_color); break;
    case P_COLUMN_RULE_WIDTH: COPY(col_rule_w); break;
    case P_COLUMN_RULE_STYLE: COPY(col_rule_style); break;
    case P_COLUMN_RULE_COLOR: COPY(col_rule_color); break;
    case P_COLUMN_FILL: COPY(col_fill_auto); break;
    case P_BREAK_INSIDE: COPY(break_avoid); break;
    case P_BREAK_BEFORE: COPY(break_before); break;
    case P_BREAK_AFTER: COPY(break_after); break;
    case P_OVERFLOW: COPY(ov_x); COPY(ov_y); break;
    case P_OVERFLOW_X: COPY(ov_x); break;
    case P_OVERFLOW_Y: COPY(ov_y); break;
    case P_OBJECT_FIT: COPY(obj_fit); break;
    case P_OBJECT_POSITION: COPY(obj_px); COPY(obj_py); break;
    case P_BACKGROUND_SIZE: COPY(bg_size); COPY(bg_w); COPY(bg_h); break;
    case P_BACKGROUND_POSITION: COPY(bg_px); COPY(bg_py); break;
    case P_BACKGROUND_POSITION_X: COPY(bg_px); break;
    case P_BACKGROUND_POSITION_Y: COPY(bg_py); break;
    case P_BACKGROUND_REPEAT: COPY(bg_norep_x); COPY(bg_norep_y); break;
    case P_BACKGROUND_ORIGIN: COPY(bg_origin); break;
    case P_GRID_TEMPLATE_COLUMNS: COPY(grid_cols); break;
    case P_GRID_TEMPLATE_ROWS: COPY(grid_rows); break;
    case P_GRID_AUTO_ROWS: COPY(grid_auto_rows); break;
    case P_GRID_TEMPLATE_AREAS: COPY(grid_areas); break;
    case P_GRID_AUTO_FLOW: COPY(grid_dense); COPY(grid_col_flow); break;
    case P_GRID_TEMPLATE: COPY(grid_cols); COPY(grid_rows); COPY(grid_areas); break;
    case P_GRID_COLUMN: COPY(gc_start); COPY(gc_end); COPY(gc_span); break;
    case P_GRID_COLUMN_START: COPY(gc_start); COPY(gc_span); break;
    case P_GRID_COLUMN_END: COPY(gc_end); COPY(gc_span); break;
    case P_GRID_ROW: COPY(gr_start); COPY(gr_end); COPY(gr_span); break;
    case P_GRID_ROW_START: COPY(gr_start); COPY(gr_span); break;
    case P_GRID_ROW_END: COPY(gr_end); COPY(gr_span); break;
    case P_GRID_AREA: COPY(grid_area); COPY(gr_start); COPY(gr_end); COPY(gr_span); COPY(gc_start); COPY(gc_end); COPY(gc_span); break;
    case P_BACKGROUND_IMAGE: COPY(gr); COPY(bg_url); COPY(bg_img); break;
    case P_BACKGROUND_COLOR: COPY(bg); break;
    case P_BACKGROUND: COPY(bg); COPY(gr); COPY(bg_url); COPY(bg_img); COPY(bg_size); COPY(bg_w); COPY(bg_h); COPY(bg_px); COPY(bg_py); COPY(bg_norep_x); COPY(bg_norep_y); COPY(bg_origin); break;
    case P_FLOAT: COPY(floated); break;
    case P_COLOR: COPY(color); break;
    case P_FONT_SIZE: COPY(font_size); break;
    case P_FONT_WEIGHT: COPY(bold); break;
    case P_FONT_STYLE: COPY(italic); break;
    case P_FONT_FAMILY: COPY(family); break;
    case P_FONT: COPY(font_size); COPY(bold); COPY(italic); COPY(family); COPY(lh); COPY(lh_kind); break;
    case P_LINE_HEIGHT: COPY(lh); COPY(lh_kind); break;
    case P_TEXT_ALIGN: COPY(text_align); COPY(center_blocks); break;
    case P_TEXT_DECORATION: COPY(decoration); COPY(deco_color); COPY(deco_style); break;
    case P_TEXT_DECORATION_LINE: COPY(decoration); break;
    case P_DECO_THICKNESS: COPY(deco_thickness); break;
    case P_UNDERLINE_OFFSET: COPY(underline_offset); break;
    case P_TEXT_DECORATION_COLOR: COPY(deco_color); break;
    case P_TEXT_DECORATION_STYLE: COPY(deco_style); break;
    case P_LETTER_SPACING: COPY(letter_spacing); break;
    case P_WORD_SPACING: COPY(word_spacing); break;
    case P_WORD_BREAK: COPY(word_break); break;
    case P_OVERFLOW_WRAP: COPY(word_break); break;
    case P_TEXT_OVERFLOW: COPY(text_ellipsis); break;
    case P_FONT_VARIANT: COPY(small_caps); break;
    case P_TABLE_LAYOUT: COPY(table_fixed); break;
    case P_CAPTION_SIDE: COPY(caption_bottom); break;
    case P_EMPTY_CELLS: COPY(empty_hide); break;
    case P_LIST_STYLE_POSITION: COPY(list_inside); break;
    case P_LIST_STYLE_IMAGE: COPY(ls_url); COPY(ls_img); break;
    case P_TAB_SIZE: COPY(tab_size); break;
    case P_TEXT_ALIGN_LAST: COPY(text_align_last); break;
    case P_LINE_CLAMP: COPY(line_clamp); break;
    case P_COUNTER_RESET: COPY(counter_reset); break;
    case P_COUNTER_INCREMENT: COPY(counter_inc); break;
    case P_CURSOR: COPY(cursor); break;
    case P_POINTER_EVENTS: COPY(pointer_none); break;
    case P_USER_SELECT: COPY(select_none); break;
    case P_BACKFACE: COPY(backface_hidden); break;
    case P_FILTER: COPY(filter); break;
    case P_BACKDROP: COPY(backdrop); break;
    case P_CLIP_PATH: COPY(clip_path); break;
    case P_BLEND: COPY(blend); break;
    case P_CONTAINER: case P_CONTAINER_NAME: case P_CONTAINER_TYPE: COPY(cq_type); COPY(cq_name); break;
    case P_OUTLINE: COPY(outline_style); COPY(outline_w); COPY(outline_color); break;
    case P_OUTLINE_STYLE: COPY(outline_style); break;
    case P_OUTLINE_WIDTH: COPY(outline_w); break;
    case P_OUTLINE_COLOR: COPY(outline_color); break;
    case P_OUTLINE_OFFSET: COPY(outline_offset); break;
    case P_TEXT_SHADOW: COPY(ts_x); COPY(ts_y); COPY(ts_blur); COPY(ts_color); break;
    case P_TEXT_TRANSFORM: COPY(text_transform); break;
    case P_TEXT_INDENT: COPY(text_indent); break;
    case P_WHITE_SPACE: COPY(white_space); break;
    case P_VERTICAL_ALIGN: COPY(vertical_align); break;
    case P_LIST_STYLE: COPY(list_style); COPY(list_inside); COPY(ls_url); COPY(ls_img); break;
    case P_LIST_STYLE_TYPE: COPY(list_style); break;
    case P_ANCHOR_NAME: COPY(anchor_name); break;
    case P_POSITION_ANCHOR: COPY(position_anchor); break;
    case P_POSITION_AREA: case P_POSITION_TRY: case P_POSITION_TRY_FALLBACKS: COPY(anchor); break;
    case P_WIDTH: COPY(width); COPY(fit_w); break;
    case P_HEIGHT: COPY(height); COPY(fit_h); break;
    case P_MIN_WIDTH: COPY(min_width); break;
    case P_MAX_WIDTH: COPY(max_width); break;
    case P_MIN_HEIGHT: COPY(min_height); break;
    case P_MAX_HEIGHT: COPY(max_height); break;
    case P_ASPECT_RATIO: COPY(aspect_w); COPY(aspect_h); break;
    case P_BOX_SIZING: COPY(box_sizing_border); break;
    case P_VISIBILITY: COPY(hidden); break;
    case P_BORDER_SPACING: COPY(border_spacing); break;
    case P_BORDER_COLLAPSE: COPY(border_spacing); break;
    case P_POSITION: COPY(positioned); COPY(position); break;
    case P_BOX_SHADOW: COPY(sh_color); COPY(sh_x); COPY(sh_y); COPY(sh_blur); COPY(sh_spread); break;
    case P_TRANSFORM: COPY(tx); COPY(ty); COPY(xf); break;
    case P_PERSPECTIVE: COPY(perspective); break;
    case P_PERSPECTIVE_ORIGIN: COPY(pox); COPY(poy); break;
    case P_TRANSFORM_ORIGIN: COPY(tox); COPY(toy); COPY(toz); break;
    case P_Z_INDEX: COPY(z_set); COPY(z_index); break;
    case P_CLEAR: COPY(clear); break;
    case P_TOP: COPY(top); break;
    case P_RIGHT: COPY(right); break;
    case P_BOTTOM: COPY(bottom); break;
    case P_LEFT: COPY(left); break;
    case P_INSET: COPY(top); COPY(right); COPY(bottom); COPY(left); break;
    case P_OPACITY: COPY(invisible); COPY(opacity); break;
    case P_TRANSITION: case P_TRANSITION_PROPERTY: case P_TRANSITION_DURATION: case P_TRANSITION_TIMING:
    case P_TRANSITION_DELAY: case P_TRANSITION_BEHAVIOR: case P_ANIMATION: case P_ANIMATION_NAME: case P_ANIMATION_DURATION:
    case P_ANIMATION_TIMING: case P_ANIMATION_DELAY: case P_ANIMATION_ITERATION: case P_ANIMATION_DIRECTION:
    case P_ANIMATION_FILL: case P_ANIMATION_STATE: COPY(raw_motion); break;
    case P_MASK: COPY(invisible); break;
    case P_CLIP: COPY(invisible); break;
    case P_MARGIN: COPY(margin); break;
    case P_PADDING: COPY(padding); break;
    case P_BORDER_RADIUS: COPY(radius); break;
    case P_BORDER_WIDTH: COPY(border_w); break;
    case P_BORDER_STYLE: COPY(border_style); break;
    case P_BORDER_COLOR: COPY(border_color); break;
    case P_MARGIN_TOP: COPY(margin[0]); break;
    case P_MARGIN_RIGHT: COPY(margin[1]); break;
    case P_MARGIN_BOTTOM: COPY(margin[2]); break;
    case P_MARGIN_LEFT: COPY(margin[3]); break;
    case P_PADDING_TOP: COPY(padding[0]); break;
    case P_PADDING_RIGHT: COPY(padding[1]); break;
    case P_PADDING_BOTTOM: COPY(padding[2]); break;
    case P_PADDING_LEFT: COPY(padding[3]); break;
    case P_BORDER_TL_RADIUS: COPY(radius[0]); break;
    case P_BORDER_TR_RADIUS: COPY(radius[1]); break;
    case P_BORDER_BR_RADIUS: COPY(radius[2]); break;
    case P_BORDER_BL_RADIUS: COPY(radius[3]); break;
    case P_BORDER: COPY(border_w); COPY(border_style); COPY(border_color); break;
    case P_BORDER_TOP: COPY(border_w[0]); COPY(border_style[0]); COPY(border_color[0]); break;
    case P_BORDER_TOP_WIDTH: COPY(border_w[0]); break;
    case P_BORDER_TOP_STYLE: COPY(border_style[0]); break;
    case P_BORDER_TOP_COLOR: COPY(border_color[0]); break;
    case P_BORDER_RIGHT: COPY(border_w[1]); COPY(border_style[1]); COPY(border_color[1]); break;
    case P_BORDER_RIGHT_WIDTH: COPY(border_w[1]); break;
    case P_BORDER_RIGHT_STYLE: COPY(border_style[1]); break;
    case P_BORDER_RIGHT_COLOR: COPY(border_color[1]); break;
    case P_BORDER_BOTTOM: COPY(border_w[2]); COPY(border_style[2]); COPY(border_color[2]); break;
    case P_BORDER_BOTTOM_WIDTH: COPY(border_w[2]); break;
    case P_BORDER_BOTTOM_STYLE: COPY(border_style[2]); break;
    case P_BORDER_BOTTOM_COLOR: COPY(border_color[2]); break;
    case P_BORDER_LEFT: COPY(border_w[3]); COPY(border_style[3]); COPY(border_color[3]); break;
    case P_BORDER_LEFT_WIDTH: COPY(border_w[3]); break;
    case P_BORDER_LEFT_STYLE: COPY(border_style[3]); break;
    case P_BORDER_LEFT_COLOR: COPY(border_color[3]); break;
    case P_MARGIN_INLINE: COPY(margin[3]); COPY(margin[1]); break;
    case P_MARGIN_BLOCK: COPY(margin[0]); COPY(margin[2]); break;
    case P_PADDING_INLINE: COPY(padding[3]); COPY(padding[1]); break;
    case P_PADDING_BLOCK: COPY(padding[0]); COPY(padding[2]); break;
    case P_INSET_INLINE: COPY(left); COPY(right); break;
    case P_INSET_BLOCK: COPY(top); COPY(bottom); break;
    case P_BACKGROUND_CLIP: COPY(bg_clip); break;
    case P_BACKGROUND_ATTACHMENT: COPY(bg_attachment); break;
    default: break;
    }
#undef COPY
    static const int backgrounds[] = { P_BACKGROUND, P_BACKGROUND_IMAGE, P_BACKGROUND_SIZE,
        P_BACKGROUND_POSITION, P_BACKGROUND_REPEAT, P_BACKGROUND_ORIGIN, P_BACKGROUND_CLIP,
        P_BACKGROUND_ATTACHMENT, P_BACKGROUND_POSITION_X, P_BACKGROUND_POSITION_Y };
    for (int k = 0; k < 10; k++)
        if (prop == backgrounds[k]) {
            if (!k) memcpy(st->bg_values, src->bg_values, sizeof(st->bg_values));
            else st->bg_values[k] = src->bg_values[k];
            break;
        }
}

struct rollback {
    int layer, origin, scope;
    const struct rollback *next;
};

static int match_origin(const struct match *m)
{
    return m->rank == R_AUTHOR_IMPORTANT ? R_AUTHOR :
           m->rank == R_INLINE_IMPORTANT ? R_INLINE : (int)m->rank;
}

static const char *custom_cascade_value(const struct collect *c, int i,
                                        const struct style *parent,
                                        const struct rollback *skip)
{
    const struct match *m = &c->m[i];
    const char *value = m->d->value;
    if (strcasecmp(value, "revert") && strcasecmp(value, "revert-layer")) return value;
    struct rollback exclude = { !strcasecmp(value, "revert") ? -1 : m->layer,
                                 match_origin(m), m->scope, skip };
    for (int j = i - 1; j >= 0; j--) {
        const struct match *p = &c->m[j];
        if (p->pe != m->pe || p->d->prop != P_CUSTOM || strcmp(p->d->name, m->d->name)) continue;
        int hidden = 0;
        for (const struct rollback *r = &exclude; r; r = r->next)
            if (match_origin(p) == r->origin && p->scope == r->scope && (r->layer < 0 || r->layer == p->layer)) hidden = 1;
        if (!hidden) return custom_cascade_value(c, j, parent, &exclude);
    }
    return lookup_var(parent, m->d->name, (int)strlen(m->d->name));
}

static void apply_match(struct style *st, const struct style *parent, const struct collect *c,
                         int i, const struct vctx *v, int pass, const struct rollback *skip,
                         int *budget)
{
    const struct match *m = &c->m[i];
    const struct decl *d = m->d;
    for (const struct rollback *r = skip; r; r = r->next)
        if (match_origin(m) == r->origin && m->scope == r->scope && (r->layer < 0 || m->layer == r->layer)) return;
    const char *value = d->value;
    char expanded[1024];
    if (substituted(value)) {
        if (!substitute(st, value, expanded, sizeof(expanded), 0)) return;
        value = expanded;
    }
    if (strcasecmp(value, "revert-layer") && strcasecmp(value, "revert")) {
        apply(st, parent, d->prop, value, v);
        return;
    }
    if (--*budget < 0) return;
    struct rollback exclude = { !strcasecmp(value, "revert") ? -1 : m->layer,
                                 match_origin(m), m->scope, skip };
    struct style lower;
    inherit(&lower, parent);
    lower.vars = st->vars;
    struct vctx ctx = *v;
    ctx.font_size = parent->font_size;
    for (int phase = 2; phase <= pass; phase++) {
        if (phase == 3) ctx.font_size = lower.font_size;
        for (int j = 0; j < i; j++) {
            int prop = c->m[j].d->prop;
            int early = prop == P_FONT_SIZE || prop == P_FONT;
            if (c->m[j].pe != m->pe || prop == P_CUSTOM || (phase == 2) != early) continue;
            apply_match(&lower, parent, c, j, &ctx, phase, &exclude, budget);
        }
    }
    restore_property(st, &lower, d->prop);
}

static void pseudo(struct arena *a, struct node *n, const struct style *st, const struct collect *c,
                   int pe, const struct vctx *base);
static void counter_apply(const char *spec, int reset);
static int counter_calls;               /* counter_apply() since the cascade began */
static void prepare_layers(struct arena *a, struct sheet **sheets, int nsheets);

static int shadow_sheets(struct arena *a, struct node *shadow, struct sheet **sheets, int vw)
{
    int ns = 0;
    for (struct node *k = shadow; k; k = node_next(k, shadow))
        if (ns < 64 && k->sheet) sheets[ns++] = k->sheet;
        else if (k->tag == T_STYLE && k->first && ns < 64)
            sheets[ns++] = css_parse(a, k->first->text, k->first->len, vw);
    if (shadow->adopted_text && ns < 64)
        sheets[ns++] = css_parse(a, shadow->adopted_text, (int)strlen(shadow->adopted_text), vw);
    return ns;
}

/* The rules that matched, as a number: the same declarations in the same order give the same
 * style (given the same parent's). Hints and inline styles are parsed afresh each time, so they
 * count by their text. */
static uint32_t match_signature(const struct collect *c, const char *hint, const char *inline_style)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < c->n; i++) {
        const struct match *m = &c->m[i];
        if (m->rank == R_HINT || m->rank == R_INLINE || m->rank == R_INLINE_IMPORTANT)
            continue;
        uint32_t parts[4] = { (uint32_t)(uintptr_t)m->d, m->rank, m->pe, (uint32_t)m->layer };
        for (int k = 0; k < 4; k++)
            h = (h ^ parts[k]) * 16777619u;
    }
    if (hint[0])
        h = (h ^ hash_str(hint)) * 16777619u;
    if (inline_style)
        h = (h ^ hash_str(inline_style) ^ 0x5bd1e995u) * 16777619u;
    return h | 1;                       /* never 0: an element never matched */
}

/*
 * Compute n's style. With check, only if the rules that match it are not
 * the ones that matched last time (css_restyle_dynamic): returns 1 if it
 * was computed, 0 if its style stays as it was.
 */
/* @keyframes for anim_style: the element's sheets, and how to apply a keyframe's declarations. */
struct kf_ctx {
    struct sheet **sheets;
    int nsheets;
    const struct style *parent;
    const struct vctx *v;
};

static const struct keyframes *kf_find(void *ctx, const char *name)
{
    struct kf_ctx *k = ctx;
    for (int i = k->nsheets - 1; i >= 0; i--)          /* the last definition wins */
        for (const struct keyframes *f = k->sheets[i] ? k->sheets[i]->keyframes : NULL; f; f = f->next)
            if (!strcmp(f->name, name))
                return f;
    return NULL;
}

static void kf_apply(void *ctx, const struct kframe *f, struct style *st)
{
    struct kf_ctx *k = ctx;
    const struct decl *d = f->decls;
    for (int i = 0; i < f->ndecls; i++) {
        if (d[i].prop == P_CUSTOM || d[i].important || prop_ap_bits(d[i].prop) == 0)
            continue;
        const char *value = d[i].value;
        char expanded[1024];
        if (substituted(value)) {
            if (!substitute(st, value, expanded, sizeof(expanded), 0))
                continue;
            value = expanded;
        }
        apply(st, k->parent, d[i].prop, value, k->v);
    }
}

/* The declarations that apply to n, in cascade order: the user agent's, hints, the author sheets
 * (the shadow tree's too), the style attribute. */
static void collect_matches(struct arena *a, struct node *n, struct sheet **sheets, int nsheets,
                            const struct vctx *base, const struct mctx *mc, struct collect *cp, char *hint,
                            int hint_size, const char **inline_out)
{
    struct collect c = *cp;
    match_sheet(&c, ua_sheet, n, R_UA, 0);
    if (mc->quirks)
        match_sheet(&c, ua_quirks, n, R_UA, 1u << 23);
    hints(n, hint, hint_size);
    if (hint[0]) {
        struct decl *d;
        int nd = parse_decls(a, hint, hint + strlen(hint), &d);
        add_decls(&c, d, nd, R_HINT, 0, 0, 0, 0x7fffffff);
    }
    for (int i = 0; i < nsheets; i++)
        if (sheets[i])
            match_sheet(&c, sheets[i], n, R_AUTHOR, (uint32_t)(i + 1) << 24);
    struct node *host = n->shadow_root ? n : n->parent && n->parent->shadow_root ? n->parent : NULL;
    if (host) {
        struct bloom *outer_bloom = cur_bloom;
        cur_bloom = NULL;               /* inside a shadow scope ancestors cross the host */
        cur_flags |= SF_NOSHARE;
        struct sheet *inner[64];
        int count = shadow_sheets(a, host->shadow_root, inner, base->vw);
        prepare_layers(a, inner, count);
        struct mctx scope = *mc;
        scope.host = host;
        scope.slotted = host == n ? NULL : n;
        c.mc = &scope;
        c.scope = -1;
        for (int i = 0; i < count; i++)
            if (inner[i]) match_sheet(&c, inner[i], n, R_AUTHOR, (uint32_t)(i+1)<<24);
        c.mc = mc;
        c.scope = 0;
        /* A shared sheet can also be adopted into another scope. Restore its layer numbering. */
        prepare_layers(a, sheets, nsheets);
        cur_bloom = outer_bloom;
    }
    const char *inline_style = node_attr(n, "style");
    if (inline_style) {
        struct decl *d;
        int nd = parse_decls(a, inline_style, inline_style + strlen(inline_style), &d);
        add_decls(&c, d, nd, R_INLINE, 0, 0, 0, 0x7fffffff);
    }
    qsort(c.m, (size_t)c.n, sizeof(struct match), cmp_match);
    *cp = c;
    *inline_out = inline_style;
}

/* The values the collected declarations give st (inheriting from parent): custom properties,
 * then font-size, then line-height and the rest. Returns 1 if an author set the color. */
static int apply_values(struct arena *a, struct node *n, struct style *st, const struct style *parent,
                        const struct collect *cp, struct sheet **sheets, int nsheets, struct vctx *v)
{
    const struct collect c = *cp;
    /* Pass 1: custom properties. */
    for (int i = 0; i < c.n; i++) {
        const struct decl *d = c.m[i].d;
        if (d->prop != P_CUSTOM || c.m[i].pe)
            continue;
        struct custom_prop *cp = arena_alloc(a, sizeof(*cp));
        if (!cp)
            break;
        cp->name = d->name;
        cp->value = custom_cascade_value(&c, i, parent, NULL);
        if (!cp->value) cp->value = "";
        /* var() inside a custom property refers to the inherited values. */
        if (substituted(cp->value)) {
            char buf2[1024];
            if (substitute(st, cp->value, buf2, sizeof(buf2), 0))
                cp->value = arena_strdup(a, buf2);
        }
        cp->next = st->vars;
        st->vars = cp;
    }

    if (!n->parent)                     /* @property initial values: there unless something sets them */
        for (int i = 0; i < nsheets; i++)
            for (const struct custom_prop *r = sheets[i] ? sheets[i]->registered : NULL; r; r = r->next) {
                struct custom_prop *cp;
                if (!lookup_var(st, r->name, (int)strlen(r->name)) && (cp = arena_alloc(a, sizeof(*cp)))) {
                    *cp = *r;
                    cp->next = st->vars;
                    st->vars = cp;
                }
            }

    v->a = a;
    v->cq_from = n->parent;
    /* Pass 2: font-size, which em units in everything else depend on. Pass 3: the rest. */
    int author_color = 0;
    int rollback_budget = 8192;
    for (int pass = 2; pass <= 3; pass++) {
        if (pass == 3) {
            v->font_size = st->font_size;
            for (int i = 0; i < c.n; i++)       /* line-height first, for lh units in the rest */
                if (c.m[i].d->prop == P_LINE_HEIGHT && !c.m[i].pe)
                    apply_match(st, parent, &c, i, v, pass, NULL, &rollback_budget);
            v->lh = line_px(st);
        }
        for (int i = 0; i < c.n; i++) {
            const struct decl *d = c.m[i].d;
            int early = d->prop == P_FONT_SIZE || d->prop == P_FONT;
            if (d->prop == P_CUSTOM || (pass == 2) != early || c.m[i].pe || (pass == 3 && d->prop == P_LINE_HEIGHT))
                continue;
            apply_match(st, parent, &c, i, v, pass, NULL, &rollback_budget);
            if (d->prop == P_COLOR && c.m[i].rank != R_UA)
                author_color = 1;
        }
    }
    return author_color;
}

/*
 * @starting-style: the style n would have if those rules applied too, for an element that had no
 * style before (just inserted, or out of display: none) to transition from. NULL if none of them
 * match it.
 */
static struct style *starting_style(struct arena *a, struct node *n, const struct style *parent,
                                    struct sheet **sheets, int nsheets, const struct vctx *base,
                                    const struct mctx *mc)
{
    struct match *buf = malloc(sizeof(struct match) * MAX_MATCHES);
    if (!buf)
        return NULL;
    const struct node *saved_subject = cur_subject;
    uint8_t saved_flags = n->style_flags;
    unsigned saved_cur = cur_flags;
    struct collect c = { .m = buf, .mc = mc };
    char hint[1024];
    const char *inline_style;
    cur_starting = 1;
    starting_hits = 0;
    cur_subject = n;
    collect_matches(a, n, sheets, nsheets, base, mc, &c, hint, sizeof(hint), &inline_style);
    cur_starting = 0;
    cur_subject = saved_subject;
    cur_flags = saved_cur;
    n->style_flags = saved_flags;
    struct style *st = NULL;
    if (starting_hits && (st = arena_alloc(a, sizeof(*st)))) {
        inherit(st, parent);
        struct vctx v = *base;
        apply_values(a, n, st, parent, &c, sheets, nsheets, &v);
        st->overlay = n->popover_open || n->modal;
    }
    free(buf);
    return st;
}

static int compute(struct arena *a, struct node *n, const struct style *parent,
                   struct sheet **sheets, int nsheets, const struct vctx *base,
                   const struct mctx *mc, struct match *buf, int check)
{
    cur_subject = n;
    cur_flags = 0;
    int calls = counter_calls;
    struct collect c = { .m = buf, .mc = mc };
    char hint[1024];
    const char *inline_style;
    collect_matches(a, n, sheets, nsheets, base, mc, &c, hint, sizeof(hint), &inline_style);
    cur_subject = NULL;
    n->style_flags = cur_flags;
    uint32_t sig = match_signature(&c, hint, inline_style);
    if (check && sig == n->match_sig && n->style && !anim_running(n))
        return 0;
    n->match_sig = sig;

    struct style *st = arena_alloc(a, sizeof(*st));
    if (!st)
        return 0;
    inherit(st, parent);
    n->style = st;
    memset(n->pseudo_st, 0, sizeof(n->pseudo_st));     /* the last cascade's styles are gone */
    memset(n->pseudo_text, 0, sizeof(n->pseudo_text));

    struct vctx v = *base;
    int author_color = apply_values(a, n, st, parent, &c, sheets, nsheets, &v);

    /* An open popover or modal dialog is in the top layer (overlay: auto, which only a transition can
     * keep a while after it leaves). */
    st->overlay = n->popover_open || n->modal;
    /* Transitions and animations put in their values now, before what depends on them. */
    finish_motion(a, st);
    if (st->ntrans || st->nanims || n->anim) {
        struct kf_ctx kc = { sheets, nsheets, parent, &v };
        struct anim_env env = { a, &kc, kf_find, kf_apply, NULL };
        if (css_starting_rules && st->ntrans && st->display != D_NONE && anim_needs_start(n))
            env.start = starting_style(a, n, parent, sheets, nsheets, base, mc);
        anim_style(n, st, &env);
        n->style_flags |= SF_NOSHARE;
        if (anim_running(n))
            n->style_flags |= SF_DYN_SELF;      /* restyled each frame (css_restyle_animated) */
    }

    if (st->counter_reset)
        counter_apply(st->counter_reset, 1);
    if (st->counter_inc)
        counter_apply(st->counter_inc, 0);

    /* Links follow <body link=...> unless a style sheet says otherwise. */
    if (n->tag == T_BODY && node_attr(n, "link")) {
        char col[64];
        uint32_t lc;
        if (legacy_color(node_attr(n, "link"), col, sizeof(col)) &&
            css_parse_color(col, (int)strlen(col), &lc) == 0)
            st->link_color = lc;
    }
    if (is_link(n) && !author_color && st->link_color)
        st->color = st->link_color;

    /* Computed values. */
    if (!n->parent)
        st->display = st->display == D_NONE ? D_NONE : D_BLOCK;
    /* Positioned boxes stay in the flow here, so the ones that are meant to be
     * hidden (closed menus, screen-reader text) must not take room there. (The top layer's are
     * laid out apart, and fade in and out from nothing.) */
    if (st->positioned && !st->overlay && (st->hidden || st->invisible ||
                           (st->width.unit == U_PX && st->width.v <= 1 && st->height.unit == U_PX &&
                            st->height.v <= 1)))
        st->display = D_NONE;
    if (st->invisible & 1)
        st->hidden = 1;
    if (st->invisible & 8)
        st->bg = TRANSPARENT, st->gr = NULL, st->bg_url = NULL;
    if (st->ov_x != st->ov_y && (!st->ov_x || !st->ov_y)) {      /* visible becomes auto beside a clipped axis */
        if (!st->ov_x)
            st->ov_x = OV_AUTO;
        else
            st->ov_y = OV_AUTO;
    }
    st->clips = st->ov_x || st->ov_y;
    if (st->floated && st->display != D_NONE)
        st->display = (st->display == D_TABLE || st->display == D_INLINE_TABLE) ? D_INLINE_TABLE
                                                                               : D_INLINE_BLOCK;
    for (int i = 0; i < 4; i++) {
        if (st->border_style[i] == BS_NONE)
            st->border_w[i] = 0;
        if (st->border_color[i] == 1)
            st->border_color[i] = st->color;
    }
    if (st->outline_style == BS_NONE)
        st->outline_w = 0;
    if (st->outline_color == 1)
        st->outline_color = st->color;
    if (st->col_rule_style == BS_NONE)
        st->col_rule_w = 0;
    if (st->col_rule_color == 1)
        st->col_rule_color = st->color;
    if (st->ts_color == 1)
        st->ts_color = st->color;
    if (st->font_size > 200)
        st->font_size = 200;
    finish_background(st, parent, &v);
    finish_transform(a, st);
    if(parent->inner==IN_GRID) {
        st->grid_cols=subgrid_names(a,st->grid_cols,parent->grid_cols,st->gc_start,st->gc_end,st->gc_span);
        st->grid_rows=subgrid_names(a,st->grid_rows,parent->grid_rows,st->gr_start,st->gr_end,st->gr_span);
    }
    if (st->filter) {                           /* drop-shadow() with no color: the text color */
        struct css_filter *f = NULL;
        for (int i = 0; i < st->filter->n; i++)
            if (st->filter->op[i].kind == FX_SHADOW && st->filter->op[i].color == 1) {
                if (!f && (f = arena_alloc(a, sizeof(*f))))
                    *f = *st->filter;
                if (f)
                    f->op[i].color = st->color;
            }
        if (f)
            st->filter = f;
    }
    if (st->backface_hidden && st->xf && st->xf->flipped)      /* turned away from the viewer */
        st->hidden = 1;
    st->font = wfont_get(st->family, st->bold, st->italic, st->font_size);
    if (st->line_clamp && st->max_height.unit == U_AUTO) {         /* -webkit-line-clamp: that many lines, the rest cut off */
        int lh = st->lh_kind == LH_PX ? st->lh : st->lh_kind == LH_FACTOR ? st->font_size * st->lh / 100
               : wfont_height(st->font);
        st->max_height.unit = U_PX;
        st->max_height.v = lh * st->line_clamp;
        st->ov_x = st->ov_y = OV_HIDDEN;
        st->clips = 1;
    }

    if (st->display != D_NONE)
        for (int pe = PE_BEFORE; pe <= PE_FIRST_LINE; pe++)
            pseudo(a, n, st, &c, pe, &v);
    if (st->display != D_NONE && st->overlay)  /* only what is in the top layer has a backdrop */
        pseudo(a, n, st, &c, PE_BACKDROP, &v);
    if (counter_calls != calls)
        n->style_flags |= SF_NOSHARE;   /* a sibling copying it would not count */
    return 1;
}

/* ---- counters: counter-reset and counter-increment, read by counter() in content ---- */

/* The document's counters, in tree order (a flat table: a counter is not scoped to the element
 * that made it, so nested lists that restart a counter in each level read the innermost one). */
static struct { char name[32]; int val; } counters[48];
static int ncounters;

static int counter_find(const char *name, int len, int make)
{
    for (int i = 0; i < ncounters; i++)
        if ((int)strlen(counters[i].name) == len && !strncmp(counters[i].name, name, (size_t)len))
            return i;
    if (!make || ncounters == (int)(sizeof(counters) / sizeof(counters[0])) || len >= (int)sizeof(counters[0].name))
        return -1;
    memcpy(counters[ncounters].name, name, (size_t)len);
    counters[ncounters].name[len] = 0;
    counters[ncounters].val = 0;
    return ncounters++;
}

/* "name", "name 5", "a 1 b -2": each counter set (reset) or advanced (increment). */
static void counter_apply(const char *spec, int reset)
{
    counter_calls++;
    const char *p = spec;
    while (p && *p) {
        while (isspace((unsigned char)*p))
            p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (p == s)
            break;
        if (p - s == 4 && !strncasecmp(s, "none", 4))
            return;
        int k = counter_find(s, (int)(p - s), 1);
        while (isspace((unsigned char)*p))
            p++;
        int amount = reset ? 0 : 1, num, used = (isdigit((unsigned char)*p) || *p == '-' || *p == '+') ? parse_num(p, &num) : 0;
        if (used) {
            amount = num / 100;
            p += used;
        }
        if (k >= 0) {
            if (reset)
                counters[k].val = amount;
            else
                counters[k].val += amount;
        }
    }
}

/* A counter's value as text: decimal, lower-alpha, upper-roman and so on. */
static void counter_format(int v, const char *style, int slen, char *out, int size)
{
    static const struct { int v; const char *s; } rom[] = {
        { 1000, "m" }, { 900, "cm" }, { 500, "d" }, { 400, "cd" }, { 100, "c" }, { 90, "xc" },
        { 50, "l" }, { 40, "xl" }, { 10, "x" }, { 9, "ix" }, { 5, "v" }, { 4, "iv" }, { 1, "i" },
    };
    int upper = slen >= 5 && !strncasecmp(style, "upper", 5);
    int alpha = slen >= 10 && (!strncasecmp(style + 6, "alpha", 5) || !strncasecmp(style + 6, "latin", 5));
    int roman = slen >= 11 && !strncasecmp(style + 6, "roman", 5);
    if ((upper || !strncasecmp(style, "lower", 5)) && alpha && v > 0) {
        char tmp[16];
        int n = 0;
        for (int x = v; x > 0 && n < 14; x = (x - 1) / 26)
            tmp[n++] = (char)((upper ? 'A' : 'a') + (x - 1) % 26);
        for (int i = 0; i < n && i < size - 1; i++)
            out[i] = tmp[n - 1 - i];
        out[n < size - 1 ? n : size - 1] = 0;
    } else if (roman && v > 0 && v < 4000) {
        int o = 0;
        for (int i = 0; i < 13; i++)
            while (v >= rom[i].v) {
                for (const char *c = rom[i].s; *c && o < size - 1; c++)
                    out[o++] = upper ? (char)(*c - 'a' + 'A') : *c;
                v -= rom[i].v;
            }
        out[o] = 0;
    } else if (slen >= 7 && !strncasecmp(style, "decimal-leading-zero", 20 < slen ? 20 : slen) && slen >= 20) {
        snprintf(out, (size_t)size, "%02d", v);
    } else if (slen == 4 && !strncasecmp(style, "disc", 4)) {
        snprintf(out, (size_t)size, "\xE2\x80\xA2");
    } else {
        snprintf(out, (size_t)size, "%d", v);
    }
}

/* counter(name[, style]) or counters(name, "sep"[, style]) at p: appends its text; returns the end. */
static const char *content_counter(const char *p, const char *end, char *out, int *o, int size)
{
    int plural = !strncasecmp(p, "counters(", 9);
    const char *a = p + (plural ? 9 : 8), *close = skip_group(strchr(p, '('), end);
    const char *stop = close > a ? close - 1 : a;
    const char *c1 = find_top(a, stop, ',');
    while (a < c1 && isspace((unsigned char)*a))
        a++;
    const char *ne = c1;
    while (ne > a && isspace((unsigned char)ne[-1]))
        ne--;
    const char *style = "decimal";
    int slen = 7;
    if (c1 < stop) {
        const char *c2 = plural ? find_top(c1 + 1, stop, ',') : stop;
        const char *s = plural ? c2 + 1 : c1 + 1;
        if (!plural || c2 < stop) {
            while (s < stop && isspace((unsigned char)*s))
                s++;
            const char *se = stop;
            while (se > s && isspace((unsigned char)se[-1]))
                se--;
            if (se > s)
                style = s, slen = (int)(se - s);
        }
    }
    int k = counter_find(a, (int)(ne - a), 0);
    char text[40];
    counter_format(k >= 0 ? counters[k].val : 0, style, slen, text, sizeof(text));
    for (const char *t = text; *t && *o < size - 1; t++)
        out[(*o)++] = *t;
    return close;
}

/* The text of a "content" value: strings, quotes, counters and attr(); NULL for none. */
static const char *content_text(struct arena *a, const struct node *n, const char *value)
{
    char out[256];
    int o = 0;
    const char *p = value, *end = value + strlen(value);
    if (!strcasecmp(value, "none") || !strcasecmp(value, "normal"))
        return NULL;
    while (p < end) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (p < end && *p != q) {
                int cp = (unsigned char)*p++;
                if (cp == '\\' && p < end) {
                    if (isxdigit((unsigned char)*p)) {
                        cp = 0;
                        for (int k = 0; k < 6 && p < end && isxdigit((unsigned char)*p); k++, p++)
                            cp = cp * 16 + hexval(*p);
                        if (p < end && *p == ' ')
                            p++;
                    } else {
                        cp = (unsigned char)*p++;
                    }
                    if (cp >= 0xE000 && cp <= 0xF8FF)
                        continue;       /* icon fonts: we have none of their glyphs */
                    if (o < (int)sizeof(out) - 4)
                        o += utf8_put(out + o, cp);
                    continue;
                }
                if (o < (int)sizeof(out) - 1)
                    out[o++] = (char)cp;
            }
            p++;
        } else if (!strncasecmp(p, "open-quote", 10) || !strncasecmp(p, "close-quote", 11)) {
            if (o < (int)sizeof(out) - 4)
                o += utf8_put(out + o, p[0] == 'o' || p[0] == 'O' ? 0x201C : 0x201D);
            p += p[0] == 'o' || p[0] == 'O' ? 10 : 11;
        } else if (!strncasecmp(p, "attr(", 5)) {
            const char *close = strchr(p, ')');
            char name[64];
            int nl = close ? (int)(close - p - 5) : 0;
            if (nl > 0 && nl < (int)sizeof(name)) {
                memcpy(name, p + 5, (size_t)nl);
                name[nl] = 0;
                const char *v = node_attr(n, name);
                for (; v && *v && o < (int)sizeof(out) - 1; v++)
                    out[o++] = *v;
            }
            p = close ? close + 1 : end;
        } else if (!strncasecmp(p, "counter(", 8) || !strncasecmp(p, "counters(", 9)) {
            p = content_counter(p, end, out, &o, (int)sizeof(out));
        } else if (*p == '(' || isalpha((unsigned char)*p)) {
            /* url() and the like: nothing we can show */
            while (p < end && !isspace((unsigned char)*p)) {
                if (*p == '(')
                    p = skip_group(p, end);
                else
                    p++;
            }
        } else {
            p++;
        }
    }
    if (!o)
        return NULL;
    return arena_strndup(a, out, (size_t)o);
}

/* The style (and for ::before and ::after the text) of one of n's pseudo-elements, if it has one:
 * generated content needs a "content"; ::placeholder, ::selection and ::marker only declarations. */
static void pseudo(struct arena *a, struct node *n, const struct style *st, const struct collect *c,
                   int pe, const struct vctx *base)
{
    const char *content = NULL, *text = NULL;
    int any = 0;
    for (int i = 0; i < c->n; i++) {
        if (c->m[i].pe != (uint32_t)pe)
            continue;
        any = 1;
        if (c->m[i].d->prop == P_CONTENT)
            content = c->m[i].d->value;
    }
    if (!any)
        return;
    if (pe <= PE_AFTER) {
        if (!content)
            return;
        const char *creset = NULL, *cinc = NULL;
        for (int i = 0; i < c->n; i++) {            /* counters the pseudo-element itself sets, before it reads them */
            if (c->m[i].pe != (uint32_t)pe)
                continue;
            if (c->m[i].d->prop == P_COUNTER_RESET)
                creset = c->m[i].d->value;
            else if (c->m[i].d->prop == P_COUNTER_INCREMENT)
                cinc = c->m[i].d->value;
        }
        if (creset)
            counter_apply(creset, 1);
        if (cinc)
            counter_apply(cinc, 0);
        text = content_text(a, n, content);
        if (!text)
            return;
    }
    struct style *ps = arena_alloc(a, sizeof(*ps));
    if (!ps)
        return;
    inherit(ps, st);
    struct vctx v = *base;
    int rollback_budget = 8192;
    for (int pass = 2; pass <= 3; pass++) {
        if (pass == 3) {
            v.font_size = ps->font_size;
            for (int i = 0; i < c->n; i++)
                if (c->m[i].pe == (uint32_t)pe && c->m[i].d->prop == P_LINE_HEIGHT)
                    apply_match(ps, st, c, i, &v, pass, NULL, &rollback_budget);
            v.lh = line_px(ps);
        }
        for (int i = 0; i < c->n; i++) {
            const struct decl *d = c->m[i].d;
            int early = d->prop == P_FONT_SIZE || d->prop == P_FONT;
            if (c->m[i].pe != (uint32_t)pe || d->prop == P_CUSTOM || (pass == 2) != early ||
                (pass == 3 && d->prop == P_LINE_HEIGHT))
                continue;
            apply_match(ps, st, c, i, &v, pass, NULL, &rollback_budget);
        }
    }
    if (pe <= PE_AFTER && (ps->display == D_NONE || (ps->positioned && (ps->hidden || ps->invisible))))
        return;
    for (int i = 0; i < 4; i++) {
        if (ps->border_style[i] == BS_NONE)
            ps->border_w[i] = 0;
        if (ps->border_color[i] == 1)
            ps->border_color[i] = ps->color;
    }
    if (ps->ts_color == 1)
        ps->ts_color = ps->color;
    if (ps->invisible & 8)
        ps->bg = TRANSPARENT;
    finish_background(ps, st, &v);
    finish_transform(a, ps);
    ps->font = wfont_get(ps->family, ps->bold, ps->italic, ps->font_size);
    n->pseudo_st[pe - 1] = ps;
    if (pe <= PE_AFTER)
        n->pseudo_text[pe - 1] = text;
}

static void layer_number(struct layer *all, struct layer *parent, int *order)
{
    for (struct layer *l = all; l; l = l->next)
        if (l->parent == parent) {
            layer_number(all, l, order);
            l->order = ++*order;
        }
}

/* Layer names belong to the whole style scope, rather than one <style>. */
static void prepare_layers(struct arena *a, struct sheet **sheets, int nsheets)
{
    struct layer *all = NULL, *last = NULL;
    for (int i = 0; i < nsheets; i++) {
        if (!sheets[i]) continue;
        for (struct layer *l = sheets[i]->layers; l; l = l->next) {
            struct layer *parent = l->parent ? l->parent->canonical : NULL;
            struct layer *found;
            for (found = all; found; found = found->next)
                if (found->parent == parent && !strcmp(found->name, l->name)) break;
            if (!found) {
                found = arena_alloc(a, sizeof(*found));
                if (!found) return;
                found->name = l->name;
                found->parent = parent;
                if (last) last->next = found;
                else all = found;
                last = found;
            }
            l->canonical = found;
        }
    }
    int order = 0;
    layer_number(all, NULL, &order);
}

int css_shared;                         /* elements styled by sharing, in all cascades so far */

/* Style sharing: an element can take its previous element sibling's style when it has the same
 * name and attributes and nothing that matched the sibling looked at what tells them apart
 * (its place among its siblings, its contents, its state: SF_NOSHARE). */
static struct node *share_source(const struct node *n)
{
    if (n->tag == T_UNKNOWN || n->value || n->shadow_root || n->shadow_host || n->adopted_text)
        return NULL;
    switch (n->tag) {
    case T_INPUT: case T_SELECT: case T_TEXTAREA: case T_BUTTON: case T_OPTION: case T_OPTGROUP:
        return NULL;
    }
    struct node *p = n->prev;
    while (p && p->type != N_ELEMENT)
        p = p->prev;
    if (!p || p->tag != n->tag || !p->style || (p->style_flags & SF_NOSHARE) || p->value || p->shadow_root ||
        p->pseudo_text[0] || p->pseudo_text[1])
        return NULL;
    const struct attr *x = n->attrs, *y = p->attrs;
    for (; x && y; x = x->next, y = y->next)
        if (strcmp(x->name, y->name) || strcmp(x->value ? x->value : "", y->value ? y->value : ""))
            return NULL;
    return x || y ? NULL : p;
}

/*
 * Style the elements under root in a pre-order walk; a display:none subtree is skipped (and
 * shares its root's style). bloom (the main tree only) holds the ancestors of the element being
 * styled. With dyn (css_restyle_dynamic), only the elements whose state or parent's style changed
 * are looked at again; returns how many elements' styles changed.
 */
static int cascade_tree(struct arena *a, struct node *root, const struct style *parent,
                        struct sheet **sheets, int nsheets, struct vctx *v,
                        const struct mctx *mc, struct match *buf, struct bloom *bloom, unsigned dyn)
{
    if (!dyn)
        prepare_layers(a, sheets, nsheets);
    cur_bloom = bloom;
    int changed = 0;
    struct node *n = root;
    while (n) {
        struct node *next = NULL;
        if (n->type == N_ELEMENT) {
            const struct style *ps = n->parent && n->parent->style ? n->parent->style : parent;
            int redone = 1;
            if (dyn) {
                int parent_changed = n->parent && n->parent->rgen == dyn;
                if (parent_changed || !n->style || (n->style_flags & SF_DYN_OTHER) ||
                    ((n->style_flags & SF_DYN_SELF) && n->dgen == dyn))
                    redone = compute(a, n, ps, sheets, nsheets, v, mc, buf, !parent_changed && n->style);
                else
                    redone = 0;
                if (redone) {
                    n->rgen = dyn;
                    changed++;
                }
            } else {
                struct node *src = mc->host ? NULL : share_source(n);
                if (src) {
                    n->style = src->style;
                    memcpy(n->pseudo_st, src->pseudo_st, sizeof(n->pseudo_st));
                    memset(n->pseudo_text, 0, sizeof(n->pseudo_text));
                    n->style_flags = src->style_flags;
                    n->match_sig = src->match_sig;
                    css_shared++;
                } else {
                    compute(a, n, ps, sheets, nsheets, v, mc, buf, 0);
                }
            }
            if (!n->parent && !n->shadow_host && n->style) {
                v->root_font_size = n->style->font_size;
                v->root_lh = line_px(n->style);
            }
            if (n->style && n->style->display == D_NONE) {
                if (redone)
                    for (struct node *k = n->first; k; k = node_next(k, n))
                        if (k->type == N_ELEMENT)
                            k->style = n->style;
            } else if (n->first) {
                next = n->first;
                if (bloom)
                    bloom_node(bloom, n, 1);
            }
        }
        if (!next) {
            struct node *p = n;
            while (p && p != root && !p->next) {
                p = p->parent;
                if (bloom && p)
                    bloom_node(bloom, p, -1);   /* its children are done */
            }
            next = p && p != root ? p->next : NULL;
        }
        n = next;
    }
    cur_bloom = NULL;
    return changed;
}

/* Shadow styles cascade in their own scope, inheriting only from their host. */
static int cascade_shadows(struct arena *a, struct node *root, struct vctx *v, const struct mctx *mc,
                           struct match *buf, int depth)
{
    int found = 0;
    if (depth > 64)
        return 1;
    for (struct node *n = root; n; n = node_next(n, root))
        if (n->shadow_root) {
            struct node *shadow = n->shadow_root;
            struct sheet *sheets[64];
            int ns = shadow_sheets(a, shadow, sheets, v->vw);
            struct mctx scope = *mc;
            scope.host = n;
            scope.slotted = NULL;
            cascade_tree(a, shadow, n->style, sheets, ns, v, &scope, buf, NULL, 0);
            cascade_shadows(a, shadow, v, mc, buf, depth + 1);
            found = 1;
        }
    return found;
}

/* What the last full cascade of each document (a page and its frames) styled, for css_restyle_dynamic(). */
struct cascade_record {
    struct node *root;
    struct sheet **sheets;
    int nsheets, vw, vh, ok;
    const struct node *state[4];        /* the :hover, :focus, :active and :focus-visible elements */
};
static struct cascade_record records[8];
static int next_record;

static struct cascade_record *cascade_record(const struct node *root, int make)
{
    for (int i = 0; i < 8; i++)
        if (records[i].root == root)
            return &records[i];
    if (!make)
        return NULL;
    struct cascade_record *r = &records[next_record];
    next_record = (next_record + 1) % 8;
    memset(r, 0, sizeof(*r));
    r->root = (struct node *)root;
    return r;
}

static void cascade_state(const struct node **s)
{
    s[0] = css_hover_node;
    s[1] = css_focus_node;
    s[2] = css_active_node;
    s[3] = css_focus_visible_node;
}

void css_cascade(struct arena *a, struct node *root, struct sheet **sheets, int nsheets,
                 int viewport_w, int viewport_h, int (*visited)(const char *href, void *ctx),
                 void *ctx)
{
    init_initial();
    ncounters = 0;
    counter_calls = 0;
    struct cascade_record *rec = cascade_record(root, root != NULL);
    if (rec)
        rec->ok = 0;
    if (!ua_sheet) {
        ua_sheet = css_parse(&ua_arena, ua_css, (int)strlen(ua_css), viewport_w);
        ua_quirks = css_parse(&ua_arena, ua_quirks_css, (int)strlen(ua_quirks_css), viewport_w);
    }
    if (!ua_sheet || !ua_quirks || !root)
        return;
    anim_cascade_begin(root);
    initial_style.font_size = ZOOM(16);
    for (int i = 0; i < 4; i++)
        initial_style.border_w[i] = ZOOM(3);
    initial_style.outline_w = ZOOM(3);
    cq_guess_w = viewport_w, cq_guess_h = viewport_h;
    struct vctx v = { ZOOM(16), ZOOM(16), viewport_w, viewport_h, a, 0, 0, NULL };
    struct mctx mc = { .visited = visited, .ctx = ctx, .quirks = root->quirks };
    struct match *buf = malloc(sizeof(struct match) * MAX_MATCHES);
    struct bloom *bloom = calloc(1, sizeof(*bloom));
    if (!buf || !bloom) {
        free(buf);
        free(bloom);
        return;
    }
    if (!++key_gen)                     /* ids and classes may have changed: hash them again */
        key_gen = 1;
    key_arena = a;
    struct sheet **combined = sheets;
    if (root->adopted_text) {
        combined = arena_alloc(a, sizeof(*combined) * (size_t)(nsheets + 1));
        if (!combined) { free(buf); free(bloom); key_arena = NULL; return; }
        if (nsheets) memcpy(combined, sheets, sizeof(*combined) * (size_t)nsheets);
        combined[nsheets++] = css_parse(a, root->adopted_text, (int)strlen(root->adopted_text), v.vw);
    }
    cascade_tree(a, root, &initial_style, combined, nsheets, &v, &mc, buf, bloom, 0);
    int shadows = cascade_shadows(a, root, &v, &mc, buf, 0);
    anim_cascade_end(root);
    free(buf);
    free(bloom);
    key_arena = NULL;
    /* Counters run in document order, and shadow trees and adopted sheets cascade
     * apart: a page with any of them is restyled whole. */
    rec->ok = !shadows && !counter_calls && !root->adopted_text;
    rec->sheets = sheets;
    rec->nsheets = nsheets;
    rec->vw = viewport_w;
    rec->vh = viewport_h;
    cascade_state(rec->state);
}

int css_restyle_dynamic(struct arena *a, struct node *root, struct sheet **sheets, int nsheets,
                        int viewport_w, int viewport_h, int (*visited)(const char *href, void *ctx),
                        void *ctx)
{
    static unsigned gen;
    struct cascade_record *rec = cascade_record(root, 0);
    if (!rec || !rec->ok || sheets != rec->sheets || nsheets != rec->nsheets || viewport_w != rec->vw ||
        viewport_h != rec->vh || !root->style || !ua_sheet || !ua_quirks)
        return -1;
    const struct node *now[4];
    cascade_state(now);
    if (!memcmp(now, rec->state, sizeof(now)))
        return 0;
    if (!++gen)
        gen = 1;
    /* Whose state changed: :hover, :active and :focus-within reach the ancestors too. */
    for (int i = 0; i < 4; i++) {
        if (now[i] == rec->state[i])
            continue;
        for (struct node *n = (struct node *)now[i]; n; n = n->parent)
            n->dgen = gen;
        for (struct node *n = (struct node *)rec->state[i]; n; n = n->parent)
            n->dgen = gen;
    }
    cq_guess_w = viewport_w, cq_guess_h = viewport_h;
    struct vctx v = { ZOOM(16), root->style->font_size, viewport_w, viewport_h, a, 0, 0, NULL };
    struct mctx mc = { .visited = visited, .ctx = ctx, .quirks = root->quirks };
    struct match *buf = malloc(sizeof(struct match) * MAX_MATCHES);
    struct bloom *bloom = calloc(1, sizeof(*bloom));
    if (!buf || !bloom) {
        free(buf);
        free(bloom);
        return -1;
    }
    key_arena = a;
    int changed = cascade_tree(a, root, &initial_style, sheets, nsheets, &v, &mc, buf, bloom, gen);
    key_arena = NULL;
    free(buf);
    free(bloom);
    memcpy(rec->state, now, sizeof(now));
    return changed > 0;
}

int css_restyle_animated(struct arena *a, struct node *root, struct sheet **sheets, int nsheets,
                         int viewport_w, int viewport_h, int (*visited)(const char *href, void *ctx),
                         void *ctx)
{
    static unsigned gen = 0x80000000u;  /* apart from css_restyle_dynamic's generations */
    struct cascade_record *rec = cascade_record(root, 0);
    if (!rec || !rec->ok || sheets != rec->sheets || nsheets != rec->nsheets || viewport_w != rec->vw ||
        viewport_h != rec->vh || !root->style || !ua_sheet || !ua_quirks)
        return -1;
    if (!++gen)
        gen = 0x80000000u;
    if (!anim_mark(root, gen))
        return 0;
    cq_guess_w = viewport_w, cq_guess_h = viewport_h;
    struct vctx v = { ZOOM(16), root->style->font_size, viewport_w, viewport_h, a, 0, 0, NULL };
    struct mctx mc = { .visited = visited, .ctx = ctx, .quirks = root->quirks };
    struct match *buf = malloc(sizeof(struct match) * MAX_MATCHES);
    struct bloom *bloom = calloc(1, sizeof(*bloom));
    if (!buf || !bloom) {
        free(buf);
        free(bloom);
        return -1;
    }
    key_arena = a;
    int changed = cascade_tree(a, root, &initial_style, sheets, nsheets, &v, &mc, buf, bloom, gen);
    key_arena = NULL;
    free(buf);
    free(bloom);
    return changed > 0;
}
