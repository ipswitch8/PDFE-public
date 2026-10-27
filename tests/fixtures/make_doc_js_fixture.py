# -*- coding: utf-8 -*-
"""Pipeline E phase-9 test fixture: a tiny PDF with a document-level
/JavaScript /Names entry containing a willSave handler that sets a
form-field value. Run once to (re)generate test_doc_js.pdf.

Output: test_doc_js.pdf (1 page, 1 text field "marker", willSave
script that writes "from-willSave" into the field).
"""
from pathlib import Path
import pikepdf
from pikepdf import Pdf, Dictionary, Array, Name, Stream, String


HERE = Path(__file__).parent
OUT = HERE / "test_doc_js.pdf"


def main() -> None:
    pdf = Pdf.new()

    # Add one blank page.
    pdf.add_blank_page(page_size=(612, 792))
    page = pdf.pages[0]

    # AcroForm with one text field "marker".
    field = pdf.make_indirect(Dictionary(
        FT=Name.Tx,
        T=String("marker"),
        V=String(""),
        Ff=0,
        Type=Name.Annot,
        Subtype=Name.Widget,
        Rect=Array([72, 72, 200, 100]),
        P=page.obj,
    ))
    page.Annots = Array([field])

    pdf.Root.AcroForm = Dictionary(
        Fields=Array([field]),
        NeedAppearances=True,
    )

    # Document-level JavaScript event hooks live in the Catalog's
    # /AA (additional actions) dict per ISO 32000-1 §12.6.3:
    #   /WS = will save, /DS = did save, /WP = will print,
    #   /DP = did print, /WC = will close
    # Each is a /JavaScript action that runs at the corresponding event.
    will_save_action = pdf.make_indirect(Dictionary(
        S=Name.JavaScript,
        JS=String(
            'this.getField("marker").value = "from-willSave";'
        ),
    ))

    pdf.Root.AA = Dictionary(WS=will_save_action)

    # Also expose the script in the /Names /JavaScript name tree so it
    # is reachable via app.execMenuItem-like flows; not strictly
    # required for document events but documents the script's name.
    name_tree = Dictionary(
        Names=Array([String("willSaveHook"), will_save_action]),
    )
    pdf.Root.Names = Dictionary(JavaScript=name_tree)

    pdf.save(str(OUT))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
