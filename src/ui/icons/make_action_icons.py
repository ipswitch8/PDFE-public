# -*- coding: utf-8 -*-
# Generates the toolbar action icons for PDFE (bugfix-3e91a7c4 #2 — the toolbars
# shipped icon-less, rendering as a "series of words"). Simple, recognizable
# monochrome line-art, 24x24, stroke-based so they stay crisp at any DPI. Output
# is a set of .svg files next to this script; src/ui/action_icons.qrc embeds them
# under :/icons/actions/<name>.svg and MainWindow sets them via QAction::setIcon.
import os

OUT_DIR = os.path.dirname(os.path.abspath(__file__))
STROKE = "#303030"

def svg(body):
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" '
        'viewBox="0 0 24 24" fill="none" stroke="%s" stroke-width="1.7" '
        'stroke-linecap="round" stroke-linejoin="round">\n%s\n</svg>\n' % (STROKE, body)
    )

# name -> inner SVG markup (paths/shapes)
ICONS = {
    # ---- file / edit / view (main toolbar) ----
    "open":     '<path d="M3 6.5A1.5 1.5 0 0 1 4.5 5h4l2 2.5H19a1.5 1.5 0 0 1 1.5 1.5v8A1.5 1.5 0 0 1 19 18.5H4.5A1.5 1.5 0 0 1 3 17z"/>',
    "save":     '<path d="M5 4h11l3 3v13H5z"/><path d="M8 4v5h7V4"/><rect x="8" y="13" width="8" height="5"/>',
    "undo":     '<path d="M8 8H15a4.5 4.5 0 0 1 0 9h-3"/><path d="M8 4 4 8l4 4"/>',
    "redo":     '<path d="M16 8H9a4.5 4.5 0 0 0 0 9h3"/><path d="M16 4l4 4-4 4"/>',
    "zoom_in":  '<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5 5"/><path d="M10.5 8v5M8 10.5h5"/>',
    "zoom_out": '<circle cx="10.5" cy="10.5" r="6"/><path d="M15 15l5 5"/><path d="M8 10.5h5"/>',
    "fit_width":'<rect x="6" y="5" width="12" height="14" rx="1"/><path d="M9 12H3M6 9l-3 3 3 3"/><path d="M15 12h6M18 9l3 3-3 3"/>',
    "fit_page": '<rect x="5" y="4" width="14" height="16" rx="1"/><path d="M9 8 7 8 7 10M15 8h2v2M9 16H7v-2M15 16h2v-2"/>',
    # ---- annotation tools ----
    "select":       '<path d="M6 4l12 7-5 1.5 2.5 5.5-2.2 1-2.6-5.5L6 18z"/>',
    "sticky_note":  '<path d="M5 5h14v9l-5 5H5z"/><path d="M14 19v-5h5"/>',
    "highlight":    '<path d="M4 20h6"/><path d="M12 4l6 6-7 7-4 1 1-4z"/><path d="M11 5l6 6"/>',
    "underline":    '<path d="M7 4v6a5 5 0 0 0 10 0V4"/><path d="M5 20h14"/>',
    "strikeout":    '<path d="M8 6a4 3 0 0 1 8 0"/><path d="M8 16a4 3 0 0 0 8 0"/><path d="M4 11h16"/>',
    "form":         '<rect x="4" y="6" width="16" height="12" rx="1"/><path d="M7 10h6M7 14h10"/><path d="M15 9l1.5 1.5L20 7"/>',
    "image":        '<rect x="4" y="5" width="16" height="14" rx="1"/><circle cx="9" cy="10" r="1.6"/><path d="M5 17l4.5-4.5L13 16l3-3 3 3"/>',
    "edit_text":    '<path d="M5 7h9M9 7v11M6.5 18h5"/><path d="M15 15l4.2-4.2a1.4 1.4 0 0 1 2 2L17 17l-2.8.6z"/>',
    "redaction":    '<rect x="4" y="8" width="16" height="8" rx="1" fill="%s" stroke="none"/><path d="M4 6.5h16M4 17.5h16"/>' % STROKE,
    "ink":          '<path d="M4 17c3 0 3-8 6-8s3 8 6 8 3-6 4-6"/>',
    "text_box":     '<rect x="4" y="5" width="16" height="14" rx="1"/><path d="M8 9h8M12 9v6"/>',
    "rectangle":    '<rect x="5" y="7" width="14" height="10" rx="1"/>',
    "circle":       '<circle cx="12" cy="12" r="7"/>',
    # GitHub #253 -- the Link annotation tool was the eleventh of eleven
    # and had no icon because nothing ever instantiated it.
    "link":         '<path d="M10.5 13.5a3.5 3.5 0 0 1 0-5l2-2a3.5 3.5 0 0 1 5 5l-1 1"/><path d="M13.5 10.5a3.5 3.5 0 0 1 0 5l-2 2a3.5 3.5 0 0 1-5-5l1-1"/>',
    "line":         '<path d="M5 19 19 5"/><circle cx="5" cy="19" r="1.3"/><circle cx="19" cy="5" r="1.3"/>',
    "stamp":        '<path d="M9 4h6v3.2a3 3 0 0 1-1 2.2l-.6 2.6h-2.8L10 9.4a3 3 0 0 1-1-2.2z"/><rect x="4" y="14" width="16" height="3.2" rx="1"/><path d="M4 20h16"/>',
    "attach_file":  '<path d="M17 8.5 10 15.5a2.5 2.5 0 0 1-3.5-3.5l7.2-7.2a4 4 0 0 1 5.7 5.7l-7.4 7.4a5.5 5.5 0 0 1-7.8-7.8L8 5"/>',
    "print_preview":'<path d="M7 8V4h10v4"/><rect x="4" y="8" width="16" height="7" rx="1"/><rect x="7" y="14" width="10" height="6"/><circle cx="17" cy="11" r=".9"/>',
}

names = []
for name, body in ICONS.items():
    path = os.path.join(OUT_DIR, "action_%s.svg" % name)
    with open(path, "w", encoding="utf-8") as f:
        f.write(svg(body))
    names.append(name)
    print("wrote", os.path.basename(path))

# Emit the .qrc so it always matches the generated set.
qrc_path = os.path.join(os.path.dirname(OUT_DIR), "action_icons.qrc")
lines = ['<!DOCTYPE RCC><RCC version="1.0">', '  <qresource prefix="/icons/actions">']
for name in names:
    lines.append('    <file alias="%s.svg">icons/action_%s.svg</file>' % (name, name))
lines.append('  </qresource>')
lines.append('</RCC>')
with open(qrc_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print("wrote", qrc_path, "with", len(names), "icons")
