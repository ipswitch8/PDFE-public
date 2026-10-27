# -*- coding: utf-8 -*-
"""
Generator for tests/fixtures/js_sandbox_probe.pdf

A single-page AcroForm with one text field "probe" whose FORMAT action
(/AA/F) runs a JavaScript sandbox probe: it writes into event.value the
`typeof` of the dangerous Acrobat APIs a hostile form script would try to reach.
Firing the format event (JavaScriptEngine::fireFormatEvent) returns that value,
giving a REAL oracle for the JS sandbox: the dangerous APIs must be absent
(`undefined`), i.e. NOT exposed to form JavaScript.

This replaces the all-EXPECT_NO_THROW sandbox suite (audit C2): a test that only
checks "did not crash" passes even if app.launchURL is fully functional. The
probe result lets the test assert the sandbox actually blocks the escape.

The format script reports a pipe-joined string:
    <typeof app.launchURL>|<typeof System>|<typeof app.openDoc>|<typeof app.alert>
so the test can assert launchURL/System/openDoc are "undefined".

Regenerate with:  python tests/fixtures/gen_js_sandbox_probe.py
"""
import os
import pikepdf

HERE = os.path.dirname(os.path.abspath(__file__))

# Probe a broad set of dangerous Acrobat APIs. The format event writes a
# pipe-joined "name=typeof" list into event.value; the test asserts the
# sandbox-escape APIs are "undefined". Kept in one script so a single
# fireFormatEvent call reports the whole sandbox surface.
_PROBE_NAMES = [
    "app.launchURL",         # open arbitrary URL
    "app.openDoc",           # open arbitrary file (path traversal)
    "app.execMenuItem",      # drive the host app
    "System",                # OS / filesystem leak (Acrobat)
    "Net",                   # network
    "SOAP",                  # network RPC
    "Collab",                # collaboration / network
    "this.exportDataObject", # write embedded file to disk
    "this.importDataObject",
    "this.submitForm",       # exfiltrate form data
    "this.getURL",           # fetch arbitrary URL
    "util.readFileIntoStream",
]
PROBE_JS = "event.value = " + " + '|' + ".join(
    "'%s=' + (typeof %s)" % (n, n) for n in _PROBE_NAMES
) + ";"


def main():
    pdf = pikepdf.new()
    page = pdf.add_blank_page(page_size=(612, 792))

    # A minimal text-field widget with a Format (/AA/F) JavaScript action.
    field = pikepdf.Dictionary(
        FT=pikepdf.Name.Tx,
        T=pikepdf.String("probe"),
        Ff=0,
        V=pikepdf.String(""),
        Type=pikepdf.Name.Annot,
        Subtype=pikepdf.Name.Widget,
        Rect=pikepdf.Array([100, 700, 300, 720]),
        AA=pikepdf.Dictionary(
            F=pikepdf.Dictionary(
                S=pikepdf.Name.JavaScript,
                JS=pikepdf.String(PROBE_JS),
            )
        ),
    )
    field_ref = pdf.make_indirect(field)
    page.Annots = pikepdf.Array([field_ref])

    pdf.Root.AcroForm = pikepdf.Dictionary(
        Fields=pikepdf.Array([field_ref]),
        NeedAppearances=True,
        DA=pikepdf.String("/Helv 0 Tf 0 g"),
    )

    out = os.path.join(HERE, "js_sandbox_probe.pdf")
    pdf.save(out)
    print("wrote", out)


if __name__ == "__main__":
    main()
