# Image placement model

How PDFE represents, edits and reads back an image drawn on a page. Written for GitHub #417 (cropping a rotated image reset its rotation) and the audit that followed: every place that writes, rewrites or derives an image placement was checked for the same fault. Each one dropped state the user had not asked to change, or showed one thing while the page held another.

The tests that hold these rules are in:
- `tests/test_image_placement_state.cpp`
- `tests/test_image_editor_state_probes.cpp`
- `tests/test_watermark_exif_state.cpp`
- `tests/test_redaction_image_state.cpp`
- `tests/test_tre_tagger_state.cpp`
- `tests/test_image_placement_model.cpp` (the rules phase 2 decided: crop composition, wrapper replacement, identity, flips, forms, undo)
- `tests/test_image_editor_ui_model.cpp` (the rules phase 3 decided: relative rotate, the Crop/Resize steps, the properties panel, insert encoding and metadata, insert undo)
- `tests/test_watermark_exif_model.cpp` (the rules phase 4 decided: watermark resources and placement, metadata stripping, extract colour)
- `tests/test_redaction_image_model.cpp` (the rules phase 5 decided: image redaction, per-mark methods, page-scoped Flatten Form, flattened images)
- `tests/test_exif_strip_bounds.cpp` (phase 5: the metadata stripper's frame-size bound and visited set)
- `tests/test_tre_tagger_model.cpp` (the rules phase 6 decided: the OCR write-back's frame, matrix, image state and identity; the tagger's regions)
- `tests/test_image_move_keeps_rotation.cpp` (GitHub #416)

The code is in `src/image/`: `ImagePlacement.h` (matrix maths and the rotation/flip/skew decomposition), `ImagePlacementScanner.{h,cpp}` (reads every `Do` of a page and splices one), `ImageContentEditor` (the edits) and `ImageManager` (enumeration, removal, undo).

## The rule

An edit to an image changes exactly what the user asked for and nothing else. Moving, resizing, rotating or cropping one placement:

- keeps its other geometry: rotation, flips and skew, and crop;
- keeps everything around it: clip, opacity (`gs`) and soft mask, marked-content tag, optional-content layer, and its position in the drawing order;
- leaves every other placement of the same image as it was.

Any image control in the UI either does what it says or is disabled.

## What a placement is

A placement is one `Do` of an image XObject in a page's content, together with:

- **M**: the full CTM in force at the `Do`, mapping the image's unit square to the page. M carries position, size, rotation, flips and skew. No field is derived and then rebuilt from a subset.
- **Crop (C)**: an optional clip rectangle in the image's own unit-square space, `[u0, v0, u1, v1]` within `[0, 1]²` (v = 0 is the image's bottom row as drawn). It is written inside the CTM, so it moves, scales and rotates with the image:

  ```
  q  <F> cm  u0 v0 (u1-u0) (v1-v0) re W n  /Im Do  Q
  ```

  This is **PDFE's placement wrapper**, the only shape the editor writes. The clip part is present only when the placement is cropped; without it, `q <F> cm /Im Do Q` is also exactly what insert writes. `F` is chosen so that the effective CTM at the `Do` is M, given the CTM `C_out` in force where the wrapper sits: `F = M × C_out⁻¹` (row-vector convention, `cm` pre-multiplies). On a plain page `C_out` is the identity and `F = M`.

- **Context**: whatever encloses the `Do`. That means the enclosing `q`/`Q`, any `gs` (opacity, soft mask) or other clip, and any marked-content (`/Figure <</MCID n>> BDC`) or optional-content (`/OC /name BDC`) section around it. It also includes its position in the page's drawing order.
- **Identity**: the XObject name plus the occurrence index of this `Do` among the page's `Do`s of that name, so that two copies of one image are two placements.

## How each edit changes it

| edit | changes | keeps |
|---|---|---|
| move | translation part of M | rest of M, C, context |
| resize | scale of M in the image's own axes | rotation, flips, skew, C, context |
| rotate | rotation of M about the visible centre | scale, flips, C, context |
| crop | C (composed with an existing crop) | M, context |

The editor reads the placement's full M from the page and derives the new one from it; it never rebuilds M from width, height and angle. In the rendered frame (below), M's linear part is decomposed as `L = K × R(θ)`: `R(θ)` is the rotation, `K` the image's own-axes part (scale, flip, skew).

- **move / resize** (`resizeImage(w, h, x, y, angle)`): `K`'s rows are rescaled to lengths `w` and `h` (the full, uncropped size), the rotation is set to `angle`, and the image's centre is put at the centre of the upright rect `(x, y, w, h)`. Flips and skew stay in `K`. Passing the enumerated `visualClockwiseDegrees` keeps the rotation (GitHub #416).
- **rotate** (`rotateImage(angle, cx, cy, w, h)`): the rotation is set to `angle` (absolute, visually clockwise), `K` is kept (rescaled to `w`, `h`), and the **visible** centre (the centre of the crop) is put at `(cx, cy)`.
- **crop** (`cropImage(rect, …)`): `rect` is given in the placement's **current visible upright frame**: the frame of the enumerated `boundingBox`, un-rotated about the visible centre. Its corners are mapped through that frame and M⁻¹ into the unit square, and the new crop is the **intersection** of the current crop and that rectangle. Crops therefore compose: a second crop narrows the first, and a crop rect larger than what is visible changes nothing; a crop never brings back a cropped-away part. An empty intersection is refused (returns false, the page is unchanged). M is not touched, so rotation, flips and skew are kept (GitHub #417). The `displayWidth`, `displayHeight`, `x`, `y` arguments are ignored; they remain for source compatibility.

## Rewriting a placement

An edit rewrites the targeted `Do` occurrence in place:
- the new `cm` (and crop clip) replaces the old one inside the existing enclosing context;
- the drawing order is unchanged;
- marked-content and optional-content sections stay around it;
- no other occurrence is touched.

How it is done: `ImagePlacementScanner` tokenises the page's raw content bytes itself (not a MuPDF filter pass, which re-serialises the whole stream), tracking `q`/`Q`/`cm` across the page's content streams, and records for each `Do` its byte range, its stream, the CTM at the `Do` and the CTM where a wrapper would sit. The edit splices the bytes of the one targeted `Do` and writes the changed stream as a **new** stream object in a copy of the `/Contents` array (the old stream or array may be shared with another page). Every other byte of the page content, including every other placement, stays exactly as it was. `/Resources` are not touched.

The reader is built for content it did not write. An inline image (`BI … ID <data> EI`) is skipped by its computed data length: an explicit `/L`, or `H × ⌈W × components × BPC / 8⌉` for unfiltered data in a device, Indexed or mask colour space. Only otherwise is it skipped up to the first whitespace-delimited `EI`, so data bytes that look like `EI` or `/Im0 Do` are not read as operators. Unbalanced `Q`, a `Do` with no name, an unterminated string and a stream cut off mid-token are survived. A `Do` whose name does not resolve to an Image XObject is never edited. Each case is pinned by `ImagePlacementStateRobustnessTest`, together with q/Q split across a `/Contents` array and a content stream shared by two pages.

- **Bare `Do`** (for example `q /GS1 gs <M> cm /Im Do Q`, or a `Do` inside `BDC … EMC`): only `/Im Do` is replaced by a new wrapper `q F cm [clip] /Im Do Q`, with `F` computed against the CTM in force at the `Do`. The enclosing `gs`, clip, marked content and layer stay around it.
- **`Do` already in a PDFE wrapper** (`q F cm /Im Do Q`, or `q F cm u0 v0 du dv re W n /Im Do Q`, with the `q` and `Q` in the same stream and nothing else between them): the whole wrapper is replaced, so repeated edits never nest and never leave a dead `re W n` behind.
- **Legacy page-space crop**: the editor before this fix wrote `q x y w h re W n <M> cm /Im Do Q`, a clip in page space **outside** the `cm`, always with an **axis-aligned** `M`. For that shape only (`M`'s b and c both 0), the scanner reports the clip converted to the unit square; the clip's corners mapped through `M⁻¹` give exactly a unit-square rect. The next edit replaces the whole legacy wrapper with the unit-space form. The same operators with a rotated or skewed `M` are someone else's clip: its inverse-mapped corners do not form a unit-square rect, so converting it would visibly change the page. It is left untouched as context, and an edit wraps only the `Do` (pinned by `ImagePlacementStateWrapperTest.RotatedThirdPartyClipIsLeftUntouched`).

Removing a placement (`ImageManager::removePlacement`, and `removeImage(index)` through it) cuts that one `Do`, or the PDFE wrapper around it, the same way. The XObject resource is removed only when nothing else looks that name up in the page's resource dict. That includes a Form XObject that inherits the page's resources (pinned by `ImagePlacementStateIdentityTest.RemovingThePagePlacementKeepsAResourceAFormStillUses`).

**A refused edit changes nothing.** The editor returns false when the placement is not found, is inside a form, does not resolve to an image, or the crop intersection is empty. `markRequiresFullRewrite` is called only after a real change. The Resize, Rotate, Crop and Remove commands then report `failed()`. `UndoManager` does not stack a failed command, keeps the redo stack, and the page is not marked dirty (pinned by `ImagePlacementStateUndoTest.RefusedEditLeavesNoUndoEntryAndACleanPage`).

The old approach removed every `Do` of the name and appended a fresh `q … Q` at the end of the page. That is what dropped the crop, the opacity, the tag and the layer; what changed the stacking; and what deleted the other copies.

## Placements inside Form XObjects

Enumeration follows Form XObjects (up to 8 levels deep, with a cycle guard), composing each form's `/Matrix` into the CTM, so an image drawn through a form is listed and selectable (GitHub #427). Such entries carry `inFormXObject = true` and `occurrence = -1`. The occurrence index counts only the page's own `Do`s, and a form has its own name space, so a form placement can never alias a page placement of the same name.

They are **not editable**. The editor, `removePlacement` and `removeImage` only address `Do`s of the page's own content and refuse a form placement (occurrence -1). MainWindow's image handlers refuse a selected form placement and pass the selected placement's occurrence to the commands (pinned by `ImagePlacementStateCommandOccurrenceTest.MainWindowEditsTheSelectedSecondCopy`, which selects copy 2 with `ImageTool::selectPlacement(name, occurrence)`). The image tool commits nothing when a form placement is dragged (`.DraggingAFormPlacementCommitsNothing`). The commands carry the occurrence through execute, undo and redo (`ImagePlacementStateCommandOccurrenceTest.*CommandEditsTheSecondCopyOnly`, `.RemoveCommandRemovesTheSecondCopyOnly`). `ImagePlacementStateIdentityTest.AFormImageWithThePageImagesNameDoesNotAliasIt` pins this with a form drawing its own `/Im0` beside the page's `/Im0`. A form is a shared resource. Rewriting its content would change every page and every other place that draws it, and copying it per page would change the page's structure behind the user's back. `ImagePlacementStateIdentityTest.ImageInsideAFormIsEnumeratedButNotEditable` pins this limit.

## Undo

Edit commands snapshot the page before they run (`ImageManager::saveContentsRef`): its `/Contents` and its `ExtGState` and `Properties` resource dicts, deep-copied into a standalone holder document so the snapshot survives a save. `restoreImage` puts all three back (and the image XObject from `saveImageRef`, as before). The editor itself never changes `/Resources`. The resource restore guarantees that undo also undoes any resource change on the way, so the opacity (`gs`) and the marked-content and layer properties come back with the content. Undo and redo of a crop restore the exact content bytes.

## Page rotation

Enumeration composes the page's `/Rotate` (and its MediaBox origin) into every placement it reports. The reported M, the visible region and the rotation are in the rendered page frame: the same frame the canvas paints and mouse input arrives in. Before this, the frame was the unrotated MediaBox.

When an edit writes a placement, it takes M in that rendered frame and applies the inverse page transform once, at the content-stream boundary. A page with `/Rotate 90` therefore behaves exactly like an unrotated page to the user. Every other consumer of placement geometry uses the same one conversion, including the OCR image-replace frame (GitHub #443, phase 6).

The rendered-frame transform is `pdf_page_transform` (CropBox origin, `/Rotate`, Y flip). In phase 2 enumeration, the editor and `ImageManager::insertImage` all use it. Insert places the image upright as the user sees the rendered page; on an unrotated page it writes exactly the matrix it wrote before.

## Image actions in the UI

Every image action works on the **selected placement** (name + occurrence), read back from the page when the action runs (`MainWindow::selectedImagePlacementOnPage`), never from a cached angle or size. After every edit, and after undo and redo, MainWindow re-reads the image tool from the page and reselects the placement by identity (`resyncImageToolPlacement`). The overlay, the handles and the properties panel therefore always show what the page holds. If the placement is gone (it was removed, or undo took an insert away), nothing is selected, the panel clears and the image actions are disabled.

- **Rotate actions are relative** (GitHub #422, #432). Image > Rotate Image... (`imageRotateAction`) turns the selected image a further 90° clockwise; twice is a half turn, and on an image at 30° it gives 120°. Image > Rotate Image 180° (`imageRotate180Action`) and Rotate Image 270° Clockwise (`imageRotate270Action`) sit next to it in the Image menu and are relative too. Each computes `angle on the page + step` and commits that through the same rotate command, so crop, flips, skew and context are kept. Only the rotation handle and the properties panel's Rotation box set an absolute angle, and both start from the angle read back from the page.
- **Crop and Resize actions keep a fixed step.** This was decided in phase 3: no dialog. The properties panel and the handles already set any size, and a crop dialog would duplicate the crop handles that are still to come.
  - **Image > Crop Image...** keeps the central 80% of what is **visible now**. The rect is given in the visible upright frame, so it composes with an existing crop: a second Crop narrows the first, from 0.1–0.9 to 0.18–0.82 of the image. Rotation, flips, skew, size and the visible centre are kept.
  - **Image > Resize Image...** halves the full size about the visible centre and keeps rotation, flips, skew and the crop (it stays at the same place in the image).
  - The menu texts keep their old labels (the UI harness finds them by label). The status tips state the step.
  - Pinned by `ImagePlacementStateUiTest.CropActionComposesWithAnExistingCropAndKeepsRotation` and `.ResizeActionHalvesTheSizeAndKeepsRotationCropAndCentre`.
- **Crop re-syncs the tool** (GitHub #431), as move, resize and rotate do. `ImageEditorStateWindowTest.E5_CropResyncsTheImageTool` passed even without this, because a crop no longer changes the rotation it checks. The crop tests above compare the tool's crop and outline with the page's, and they fail when the crop handler's resync is removed.
- **Handle resize works in the image's own frame** (GitHub #433). `ImageTool` takes the page-space drag into the image's frame (rotation `+rotationRadians`), moves the grabbed edge or corner there, and turns the centre shift back onto the page. A right-edge drag on a rotated image changes its own width, and the opposite edge or corner stays where it is on screen. Pinned by `ImageEditorStateTest.E7_RightHandleDragOfRotatedImageGrowsItsOwnWidth` and `ImagePlacementStateUiTest.TopLeftHandleDragOfRotatedImageKeepsTheOppositeCorner`.
- **Placements inside a Form XObject** (occurrence -1) are selectable and not editable. Every image action and every properties-panel control is disabled for them, with a tooltip saying the image is in a Form XObject that may be shared (`ImagePlacementStateUiTest.PanelAndActionsAreDisabledForAFormPlacement`).

### The properties panel

The panel (`ImagePropertiesPanel`) shows the selected placement as the page holds it, and every control is wired in MainWindow (GitHub #423):

| control | shows | does |
|---|---|---|
| Width, Height | the **full** (uncropped) size, `displayWidth` × `displayHeight` | resizes to that full size about the visible centre, keeping rotation, flips, skew and crop. There is no aspect lock: each box changes only its own axis. Keyboard tracking is off, so typing commits one resize (one undo entry) on Enter or focus-out, not one per keystroke (`ImagePlacementStateUiTest.TypingAWidthOrHeightCommitsOnceOnEnterOrFocusOut`). |
| Rotation | the page's angle, visually clockwise. A multiple of 90° selects 0°/90°/180°/270°. Any other angle is shown as itself (for example "30°") in an extra entry that cannot be chosen, so the box never shows 0° for a rotated image. | choosing 0°/90°/180°/270° sets that **absolute** angle (crop kept) |
| Crop... | – | the same as Image > Crop Image... |
| Extract | – | saves the image's pixels to a file through a save dialog (PNG by default; the dialog seam `pdfe::ui::testseam` is honoured), using `ImageManager::extractImageToFile`. A network target obeys the same policy as Save As: when network locations are turned off, nothing is written and the status bar says "Extract refused: network locations are turned off". Success ("Image extracted to …") and failure ("The image could not be extracted to …", for example a missing folder) are reported in the status bar. Pinned by `ImagePlacementStateUiTest.ExtractToANetworkPathIsRefusedWhenTheyAreTurnedOff`, `.ExtractThatCannotWriteSaysSo` and `.ExtractSuccessIsReported`. |
| Remove | – | the same as Image > Remove Image |

A refresh from the page never requests an edit. `setImageInfo` runs under the `updatingInfo_` guard, and the extra angle entry is display only. Pinned by `ImagePlacementStateUiTest.PanelRefreshRequestsNoEdit` and `.PanelShowsAnAngleThatIsNotAQuarterTurnWithoutEditing` (one undo after a 30° rotate returns the image to upright). With no selection every control is disabled; for a form placement every control is disabled with a tooltip. No control is ever enabled while doing nothing.

The panel's Compression line names the filter: JPEG for DCT, "JPEG 2000" for JPX, "JBIG2" for JBIG2, and "Lossless" only for none of these (GitHub #425, `ImageEditorStateDataTest.E10_JpxAndJbig2AreNotReportedAsLossless`).

## Inserting an image

`ImageManager::insertImage` decodes the source and writes a new image XObject:

- **EXIF orientation is applied** (GitHub #436). The file is read with `QImageReader::setAutoTransform(true)`, so a photo whose EXIF Orientation turns it is stored upright, the way a viewer shows it. MainWindow also uses the turned size for the dialog's default rect. Pinned by `ImageEditorStateTest.E9_ExifOrientationIsApplied` and `ImagePlacementStateUiTest.DefaultInsertRectUsesTheExifTurnedSize`.
- **The source format decides the encoding** (GitHub #435). The format is read from the file's content, not its extension (`QImageReader::setDecideFormatFromContent(true)` in `insertImage`, in the Insert Image dialog and in MainWindow's size probe). A PNG named `.jpg` is stored losslessly and gets no quality box; a JPEG named `.png` is stored as DCT and gets one. Pinned by `ImagePlacementStateInsertTest.SourceFormatIsReadFromContentNotExtension`.
  - An opaque **JPEG** source (photographic and already lossy) is re-encoded as DCT at `jpegQuality` and written as a DCT stream as is. `jpegQuality` is clamped to 0–100, so -20 encodes exactly as 0 and 150 exactly as 100 (`ImagePlacementStateInsertTest.JpegQualityIsClampedToZeroToHundred`). Without the clamp, Qt's JPEG writer encodes a negative quality differently from 0, so the low end of that test fails. Qt also caps values above 100, so the high end holds even without PDFE's clamp.
  - **Every other source** (PNG, GIF, BMP, TIFF, …) and anything with an alpha channel is stored **losslessly** (Flate, with alpha as a soft mask). Line art and screenshots therefore never pick up JPEG artefacts, and transparency is kept. `jpegQuality` does not apply to them, and the Insert Image dialog disables its quality box for such a file with a tooltip saying so.
  - Pinned by `ImageEditorStateTest.E9_JpegQualityChangesTheEncodedImage` and `ImagePlacementStateInsertTest.PngSourceStaysLosslessWhateverTheQuality`.
- **No metadata reaches the PDF.** The pixels are copied into a fresh image with no text keys or colour-space tag before encoding. The written stream therefore has no EXIF/APP1 segment (GPS, camera, timestamps), no comment segment and no `Metadata` stream, **whatever `stripExif` says**. Keeping EXIF has no defined meaning for a PDF image XObject. The dialog's "Strip image metadata" box is therefore always checked and disabled, with a tooltip explaining that metadata is always removed. Pinned by `ImagePlacementStateInsertTest.JpegSourceIsStoredAsDctWithNoMetadata`, which inserts a JPEG carrying an APP1 Exif segment and a comment with both `stripExif` values and checks the stream and the saved file.
- **The insert reports its identity** (GitHub #434): `result.imageInfo` carries the new XObject name, the `occurrence` of its `Do` and its enumeration `imageIndex`, read back from the page. MainWindow selects the inserted placement by name + occurrence.
- **Undo removes exactly what the insert added.** `InsertImageCommand::undo` restores the pre-insert content snapshot, which takes the placement away and restores every other byte. It then removes the inserted name from the page's XObject resources with `ImageManager::removeImageResourceIfUnused`. That removes it only if no page `Do`, and no `Do` inside a form that inherits the page's resources, still uses it, and it marks a full rewrite so the orphaned stream is not kept. Redo re-runs the insert. Pinned by `ImageEditorStateTest.E8_InsertUndoRemovesImageAndItsResource` and `ImagePlacementStateInsertTest.InsertReportsItsIdentityAndUndoRedoRoundTrips` (a second image on the page is left alone; redo puts the placement and the resource back).

## Reading a placement back

Enumeration reports each `Do` occurrence separately. For each it gives M, its decomposition (width, height, rotation, flip, skew), the crop C, and the visible region: the unit square clipped by C and mapped through M, in the same fitz Y-down page frame as mouse input. The page's `/Rotate` is applied so that this frame is the rendered page frame.

The selection outline and hit-testing use the visible region, so a cropped image is outlined where it can be seen. Rotation shown anywhere in the UI (overlay, properties panel) is the rotation read back from the page.

`PageImageInfo` fields (`src/image/ImageData.h`):

| field | meaning |
|---|---|
| `xobjectName`, `occurrence` | identity: the n-th `Do` of that name in the page's own content, in content order; -1 for a placement inside a Form XObject |
| `inFormXObject` | drawn inside a Form XObject: enumerated, not editable |
| `ctm` | effective CTM at the `Do` in PDF **user** space (Y-up), any form `/Matrix` composed in; unchanged meaning |
| `pageCtm` | the placement in the **rendered** fitz frame (Y-down, `/Rotate` applied): unit square → page |
| `rotationRadians` | rotation of the image's own x axis, CCW-positive in the Y-up rendered frame (visually counter-clockwise). In the Y-down frame the outline is rotated by `-rotationRadians` |
| `flippedX`, `flippedY` | the placement is mirrored (negative determinant). The same matrix is both "flip x, then rotate by r" and "flip y, then rotate by r + 180°". The pair with the smaller \|rotation\| is reported, so a plain mirror reads as `flippedX` at 0 and a plain vertical flip as `flippedY` at 0; neither reads as 180°. Past 90° the reported pair switches over; the matrix, and what is drawn, is the same either way, and the editor reads the angle back with the same rule |
| `skewRadians` | angle between the image's own axes minus 90°; 0 for a rectangle. Reported, never rebuilt from; the editor keeps it by working from the full matrix |
| `displayWidth`, `displayHeight` | the **full** (uncropped) placement size: lengths of the image's own axes |
| `cropped`, `crop` | the unit-square crop `[u0 v0 u1 v1]` read from the PDFE wrapper (or a legacy page-space crop), full square when not cropped |
| `boundingBox` | the **visible** region: an upright rect in the image's own frame, centred on the visible centre, size `visibleWidth() × visibleHeight()` = `(u1-u0)·displayWidth × (v1-v0)·displayHeight`. Rotate it about its centre by `-rotationRadians` for the on-page outline |
| `placementBox` | the same for the whole, uncropped placement; equals `boundingBox` when not cropped |

The image tool's overlay drags the visible region. `placementRectForVisibleRect` (`ImageTool.h`) converts a dragged or resized visible rect back to the full placement rect that `resizeImage` takes; for an uncropped image it is the identity. The UI wiring is described in "Image actions in the UI" above.

## Watermark, metadata stripping and extraction

Phase 4 fixed the three paths outside the image editor that write or read image XObjects: the batch watermark (`BatchEngine::applyWatermark`, `applyWatermarkExt`), the metadata stripper (`ExifStripper`) and extraction (`ImageManager::extractImage`). The rules are pinned by `tests/test_watermark_exif_state.cpp` (the phase-1 probes) and `tests/test_watermark_exif_model.cpp`.

### Batch watermark: resources

- **The page's resources are kept** (GitHub #449). Before adding anything, the watermark gives the page its own copy of its effective `/Resources`: the dict inherited from the `/Pages` node, or one shared with other pages. It then copies each category it adds to (`/ExtGState`, `/XObject`, `/Font`). Every entry the page had is still there, and a dict shared with other pages is never edited. Pages outside the range stay exactly as they were.
- **Fresh names only** (GitHub #448, #449). The watermark's ExtGState, image and font are named `PdfeWmGS`, `PdfeWmImg` and `PdfeWmF`, with a number added when the page already has that name (`PdfeWmGS1`, ...). The old fixed names `/GS0`, `/WMImg` and `/F1` replaced the page's own entries of the same name. The text watermark always uses its own Helvetica font object, never a font the page already calls `/F1`.
- **The text is a literal string.** `(`, `)` and `\` in the watermark text are escaped, so the text cannot end the string and add operators.

Pinned by `WatermarkState.W1_*`, `WatermarkState.W2_*`, `WatermarkModel.ExistingWatermarkNamesAreNotReused`, `.SharedResourcesOfOtherPagesAreNotEdited` and `.TextIsWrittenAsALiteralString`.

### Batch watermark: placement

- **Placed on the page as the user sees it** (GitHub #450). The watermark is placed in the rendered frame from `pdf_page_transform` (CropBox origin, `/Rotate`, Y down) and taken into user space by the inverse transform once, as in "Page rotation". On a `/Rotate 90` or `270` page it is centred on the rendered page and upright as seen. Before, the size came from the rotated page and the origin from the unrotated MediaBox, so it landed off-centre.
- **Size.** An image watermark is 40% of the rendered page width, aspect kept. A text watermark is set at the dialog's font size. Its box is the Helvetica advance width of the text by the font's ascent (0.718 em) and descent (−0.207 em).
- **Rotation and Position work in both modes** (GitHub #451). The item is rotated counter-clockwise (as seen) by the dialog's Rotation, about its own centre. Position "Center" centres it. "Top Left", "Top Right", "Bottom Left" and "Bottom Right" put its rotated bounding box in that corner of the rendered page, 5% of the shorter page side from the edges. Before, the image branch read neither value, and text was never positioned. The dialog's Rotation and Position controls stay enabled in both modes; their tooltips say they apply to the text or the image.
- **Default rotation per mode.** Until the user changes Rotation, the dialog sets it to the mode's default when the mode changes: 45° for text, 0° (upright) for an image. That keeps an image watermark upright by default, as it always was. Once the user has set a Rotation, switching modes keeps it. `WatermarkDialog::config()` and `extConfig()` return exactly what Apply hands to the engine. The engine itself has no per-mode default: it applies `WatermarkConfig::rotation` as given.
- **What users will notice.** An image watermark is still upright by default, but the dialog's Rotation and Position now apply to it. A centred text watermark is centred on its measured width and rotated about its centre. Before, it started two font sizes left of the centre and turned about that start point.

Pinned by `WatermarkState.W3_*`, `WatermarkState.W4_*`, `WatermarkModel.Image*`, `WatermarkModel.Text*`, `WatermarkModel.DialogConfigReachesTheEngine` and `WatermarkDialogImageMode.*`.

### Metadata stripping (`ExifStripper`)

Pixels carry no metadata, so the stripper no longer decodes well-formed images (GitHub #452, #453). It used to decode every image, copy the pixels through a `QImage` and add a new image in its place. That dropped `/SMask`, `/Mask` and `/ImageMask`, and read a CMYK pixmap as RGBA, with K as alpha. It now removes metadata only where a PDF image keeps it:

- **The image dictionary.** `/Metadata` (XMP), `/PieceInfo`, `/LastModified`, `/OPI` (the path of the original high-resolution file), `/Name` and `/ID` are deleted.
- **JPEG data (`DCTDecode`).** Application and comment segments are cut out without decoding: APP1 (EXIF, XMP), APP13 (Photoshop/IPTC), the other APPn segments and COM. This includes segments after a scan. A decoder needs three of them, and they are kept, cut to their standard size:
  - **APP0 JFIF** keeps only its 14-byte header (segment length 16), with the thumbnail size set to 0x0. A JFIF segment can carry an RGB thumbnail of the original, possibly uncropped, image, and bytes can be appended after it; both are cut. A JFXX extension APP0 (a thumbnail of its own) and any other APP0 are dropped.
  - **APP14 Adobe** keeps only its 12-byte payload (segment length 14). It says whether CMYK is stored inverted.
  - **APP2 `ICC_PROFILE`** is kept whole when its chunk header is valid: `ICC_PROFILE\0`, a sequence number from 1 to the chunk count, and a count of at least 1. Otherwise it is dropped. Known limit: the profile body is kept as it is, and a profile can hold a description or copyright tag.
  - A JFIF or Adobe segment shorter than its standard payload is dropped (a decoder ignores it).

  A segment that was cut gets its length rewritten, and the image counts as stripped. The scan data and tables are copied byte for byte. Filters in front of `DCTDecode` (for example Flate) are undone, and the stream is stored as plain `DCTDecode` with that filter's `DecodeParms`. The JPEG ends at its EOI marker. Anything stored after it (MPF secondary images, thumbnails, appended EXIF) is dropped.
- **Data that does not parse cleanly.** Some JPEG or JP2 data cannot be cut losslessly: a segment length that does not land on a marker, bytes where a marker must be, data cut off before the scan, or a broken box structure. The stripper decodes such an image with MuPDF and stores its pixels as a fresh Flate stream. Gray, RGB and CMYK stay in that device colour space (CMYK is not turned into RGB), and anything else becomes RGB. An ICCBased or other non-device colour space is therefore replaced by DeviceGray, DeviceRGB or DeviceCMYK. The decoder has already applied `/Decode`, so it is removed. `/SMask` and a stream `/Mask` are kept. This fallback has two known limits. A colour-key `/Mask` array is kept as it is, and it stays correct only for 8-bit samples with the default `/Decode`; other cases may mask the wrong colours. A JPX image whose transparency comes from `/SMaskInData` loses that transparency, because the fresh Flate stream has no embedded alpha and `/SMaskInData` is removed. JPEG data cut off inside a scan is not re-encoded: every segment before the cut has been seen and removed.
  - **Size cap.** The fallback decodes at most 100 megapixels, and at most 4 bytes of samples per pixel (400 MB; `kMaxFallbackPixels` in `ExifStripper.cpp`). The limit is checked before anything is decoded, against the dictionary's `/Width` x `/Height` x colour components, and for JPEG data against every byte pair in the data that looks like a frame header (SOFn), because the JPEG decoder works from the frame header. The search does not trust segment lengths and does not stop at the first `FF DA` (start of scan): those two bytes can sit inside a COM or APPn payload ahead of the real frame header (phase 5; before, such a comment hid the frame from the bound). Entropy-coded data never holds `FF C0`..`FF CF`, so a pair that only looks like a frame header makes the bound stricter, not looser. A defensive guard also refuses to decode DCT data whose encoded bytes were not obtained (`enc` null in `reencodeImagePixels`); with MuPDF 1.26 it is not reachable (a last DCT filter always yields JPEG compression parameters, and a failed read of a buried filter's input throws first, so the image is reported failed), so no test pins it. A larger image is reported as **failed** ("could not be processed"), so a decompression bomb never allocates gigabytes. The app has no general decode limit to reuse. The JPX gate's 600-megapixel bound (`core/JpxSecurityGate.h`) applies only inside MuPDF's JPX decoder, and the fallback holds the decoded pixmap, a converted copy and the raw samples at once, so it uses a smaller bound. For comparison, an A3 page scanned at 600 dpi is about 70 megapixels.
  - **JPX decoding off.** While JPX decoding is off (the default, `core/JpxSecurityGate.h`), MuPDF returns a 1x1 placeholder for any JPX image. The fallback therefore refuses JPX data in that state: the image is reported as failed and left as it is, and is never replaced by the placeholder.
  - **A JPEG or JPX filter that is not the last filter** in `/Filter` is treated the same way. The stored bytes are not the JPEG there, so they cannot be cut losslessly; the image is decoded and re-encoded, or reported as failed. It is never reported as already clean while untouched. The size cap applies here too (phase 5; before, a buried DCT filter had no frame check at all): the bytes the DCT filter reads, that is the stored data with only the filters in front of it undone, are read (at most 64 MB, `kMaxBuriedCodecInput`; more is reported as failed) and scanned for frame headers before anything is decoded.
- **JPEG 2000 data (`JPXDecode`).** A JP2 file keeps only the boxes a decoder needs (a whitelist):
  - at the top level: the signature box `jP  `, `ftyp`, `rreq`, the `jp2h` header superbox and the `jp2c` codestream;
  - inside `jp2h`: `ihdr`, `bpcc`, `colr`, `pclr`, `cmap`, `cdef` and the `res ` superbox;
  - inside `res `: `resc` and `resd`.

  Everything else is dropped at every level: `xml ` (XMP), `uuid`, `uinf`, `jp2i`, `asoc`, `lbl `, `free` and any private box. The JPX-only `jpch` and `jplh` boxes are dropped as well. MuPDF decodes through OpenJPEG, which reads only the signature, `ftyp`, `jp2h` and `jp2c`, so they do not matter to the picture. A filtered superbox gets its length rewritten. The box structure is checked for bounds at every level: a header cut off, a length below the header size or past the end of the enclosing box. A length of 0 runs to the end of the enclosing box, and a length of 1 is followed by a 64-bit XLBox. A broken structure makes the data malformed (see the fallback). A bare codestream has no boxes and is left alone. Known limits: a `colr` box can hold an ICC profile, which is kept whole like an APP2 profile, and comment (COM) marker segments inside the codestream are not looked at.
- **Masks and alternates.** An image's `/SMask` and a stream `/Mask` are images too, and are treated the same way. So is a mask's own mask, down to three levels, and the `/Image` of every entry in `/Alternates`. Each image reached this way is visited once per page walk, by object number (a visited set, phase 5). Before, only the depth limit ended the recursion, so two images whose `/Alternates` listed each other N times cost on the order of N^3 visits; now each is visited once.
- **What is visited.** `stripPageImagesReport` (and so `stripAllImagesReport`) visits every image in the page's `/XObject` resources and in the Form XObjects listed there, recursively, down to 8 levels of forms. Each image and each form is visited once per page, by object number, so a shared form is visited once and a form that lists itself ends the recursion.
- **Page thumbnails.** The page's `/Thumb` is removed. It is a picture of the page as it was, so it can show content that has since been removed or cropped away. Viewers regenerate thumbnails, and `/Thumb` was not used anywhere in `src/` when checked (2026-11-18). The report counts removed thumbnails, and the message says "N page thumbnail(s) removed."
- **Not visited.** Inline images (`BI` ... `ID` ... `EI`) are **not** stripped: the stripper does not rewrite content streams. An inline image with JPEG or JPEG 2000 data can carry application segments, so such images are counted (on the page and inside its forms) as `StripReport::inlineNotProcessed`. The message then says "N inline JPEG image(s) were not processed; their metadata may remain.", and Tools > Strip Image Metadata shows it as a warning. Inline images with other filters hold bare samples and are not counted. Also not visited: images inside patterns, Type 3 glyphs and annotation appearance streams, and the page's own `/Metadata`, which describes the page, not an image.

Everything else in the image dictionary stays, including the soft mask, the stencil flag, `/Decode` and the colour space. So transparency and CMYK colours are exactly what they were. An image with nothing to strip is not touched at all. Raw, Flate, LZW, RunLength, CCITT and JBIG2 data hold bare samples and have no metadata to strip.

Each image has one of three outcomes: **stripped** (metadata was found and removed), **already clean** (nothing to remove, left untouched) or **failed** (it could not be stripped, for example data that is malformed and cannot be decoded either, so its metadata may remain). `stripPageImagesReport` and `stripAllImagesReport` return the three counts, and `stripImageResult` gives the outcome for one image. The older `stripPageImages`, `stripAllImages` and `stripImage` keep their meaning: the number of images free of metadata afterwards (stripped plus already clean), or true for one such image. A failed image is never counted. Tools > Strip Image Metadata shows `ExifStripper::describe`: "Metadata removed from N image(s).", then "N image(s) had no metadata to remove." and, as a warning, "N image(s) could not be processed; their metadata may remain." The result box is a `QMessageBox` named `stripImageMetadataResultDialog`. It is a warning when any image failed or any inline JPEG image was skipped, and an information box otherwise. Pinned by `ExifStripUi.*`, which runs the real menu action in a `MainWindow`. Every call still marks the document for a full rewrite, so the original bytes do not survive in an incremental save. `ExifStripModel.EveryStripCallMarksTheDocumentForFullRewrite` checks `sessionRequiresFullRewrite()` after each of the six public strip calls. `ExifStripModel.SaveToTheSameFileDoesNotKeepTheOriginalExif` strips a document, saves it to the same file with `Document::save`, which appends incrementally unless a full rewrite is required, and checks that no original EXIF or XMP bytes remain. `ExifStripModel.InformationalEntriesAreDeletedFromTheSavedImage` sets all six informational entries, including an `/OPI` dictionary holding a file path, and checks that each is gone from the saved image and that the path is not in the file.

Pinned by `ExifState.X1_*`, `ExifState.X2_CmykColoursSurviveStrip` and `ExifStripModel.*`. Those tests check that after a strip, EXIF, XMP, IPTC and comments are gone from the stored JPEG, from the dictionary and from the saved file; that masks, the Adobe marker and the rendered pixels are unchanged; that a JP2 loses only its xml box; and that a JPEG with only JFIF, and a Flate image with a soft mask, keep their exact bytes. They also cover a trailer after EOI, a bad segment length (RGB with a soft mask, and Adobe CMYK, which stays CMYK), data cut off inside and before the scan, an undecodable image reported as failed, and the counts and message per outcome. The security-audit remediation tests pin the rest. A JFIF thumbnail, bytes appended to it, a JFXX segment and Adobe padding are cut, and the render is unchanged. An invalid ICC chunk is dropped and a valid one kept. Private boxes at the top level of a JP2 and inside `jp2h` are removed, and the image still decodes to the same picture. A malformed JP2 is not replaced while JPX decoding is off. A dictionary or a JPEG frame over the size cap is reported as failed, without decoding. Images inside Form XObjects (shared and cyclic) and in `/Alternates`, and a mask's own mask, are stripped. A page thumbnail is removed. Inline JPEG images are counted and reported. A buried DCT filter is never reported clean. Phase 5 adds `ExifStripBounds.*`: a 12000 x 12000 frame behind an `FF DA` pair in a comment is reported failed in under a second, one behind a buried DCT filter (after a Flate) in under 250 ms, and two JPEG images whose `/Alternates` list each other 128 times each are stripped in under a second, both losing their EXIF. The two time bounds are what tell the fix from its absence: without the buried-filter bound the image is still reported failed, after about a second of decoding, and without the visited set the strip took about 8 s (measured by the phase-5 mutation checks); with them each takes a few milliseconds. `ExifStripBounds.BuriedDctInputOverTheCapIsNotProcessed` pins the 64 MB input cap: a buried-DCT JPEG (behind Flate, with a pass-through Crypt filter after it) is re-encoded with 1 MB of zero padding after its EOI, and reported failed and left as it was with 70 MB.

### Extraction (`ImageManager::extractImage`)

Only 8-bit gray and RGB samples are copied into the `QImage` as they are (GitHub #439). An image in any other colour space (CMYK, including Adobe-inverted DCT CMYK, Lab, ICC-based, DeviceN) is converted to RGB with `fz_convert_pixmap`, the way the page renders it. Before, a 4-component CMYK pixmap was read as RGBA. A pixmap with alpha is copied as premultiplied RGBA. Pinned by `ExtractState.R1_ExtractedCmykImageMatchesRenderedColour` and `ImageDataExtractModel.*`. Those include an ICCBased `/N 4` image, a Lab image and a DeviceN image, each checked against the rendered colour, and a JPX image with an alpha channel. That image extracts as premultiplied RGBA, so a half-transparent pixel reads back with its full colour, not halved. PDFE's MuPDF is built without an ICC engine (`FZ_ENABLE_ICC=0` in `vcpkg-overlay-ports/libmupdf`), so an ICCBased space is drawn and extracted as the device space its `/N` names, and its profile is not read. A pixmap with alpha comes only from image data that carries alpha itself, such as a JPX image with an alpha channel. A `/SMask` is separate and is not part of the extracted pixmap: an extract returns the image's own samples and does not apply its soft mask (decided in phase 6, see the row below).

## Redaction and flatten

Phase 5 fixed the redaction and flatten rows (GitHub #440, #441, #442, #437). The rules are pinned by `tests/test_redaction_image_state.cpp` (the phase-1 probes) and `tests/test_redaction_image_model.cpp`.

### Redacting an image (`RedactionManager::applyPageRedactions`)

- **Pixels is the default** (GitHub #440). `RedactionApplyOptions::imageMethod` is now `Pixels`; it was `Remove`, so a mark over a corner of a full-page scan deleted the whole scan. In Pixels mode MuPDF's redaction filter (`pdf_redact_page` with `PDF_REDACT_IMAGE_PIXELS`) handles each `Do` of an image that a mark touches:
  - A mark that covers the whole image (all four corners of its placement) removes it, as before.
  - Otherwise the image is decoded, the pixels under the mark are set to white (with the black box drawn over them, when black boxes are on), and the result is stored as a **new** Flate image XObject that this `Do` now draws. The original image object is no longer drawn by this placement. The document is marked for a full rewrite with garbage collection, so an original that nothing else draws is not in the saved file.
  - The soft mask (`/SMask`, or a stream `/Mask`) is blanked the same way: under the mark it becomes fully opaque, so the shape it held there is gone too. A 1-bit stencil `/Mask` and a colour-key `/Mask` array are handled the same way: under the mark the placement shows blank (opaque) and outside it the masked image renders as before, and no image object in the saved file still holds the original samples or the original stencil. Pinned by `RedactionImageModel.SoftMaskIsBlankedUnderTheMarkToo`, `.StencilMaskIsRedactedUnderTheMarkToo` and `.ColourKeyMaskedImageIsRedactedUnderTheMark`.
  - JPX and JBIG2 images are decoded and stored as Flate like any other. While JPX or JBIG2 decoding is off (`core/JpxSecurityGate.h`, `core/Jbig2SecurityGate.h`), MuPDF reads such an image as a blank placeholder, so a mark that touches it replaces the whole image with the blanked placeholder. The original coded data is not kept. That loses the rest of the image too, exactly as `Remove` did, and never leaves the data behind. `RedactionImageModel.Jbig2ImageIsPixelRedacted` checks both states.
  - Image data that cannot be decoded is never left in place as if it had been redacted. MuPDF's decoders are lenient: damaged data decodes to whatever can be read (the rest padded), and that is what is blanked and stored in place of the original. A decoder error that does throw propagates out of `pdf_redact_page`, and `applyAllRedactions` then reports `success = false` with the message. `RedactionImageModel.UndecodableImageIsNeverLeftInPlace` (a DCT stream of garbage under a partial mark) accepts either outcome and checks that, when the apply succeeds, the original bytes are not in the saved file; on this build the apply succeeds.
- **The box is mapped through the full placement.** The filter maps the mark through the inverse of the image's complete CTM at its `Do`: the matrix the placement model reads (`ImagePlacementScanner`, `PageImageInfo::pageCtm`), including rotation, flips and skew, and the enclosing Form XObject's `/Matrix`. A PDFE crop (the unit-square clip inside the `cm`) and a page-space clip only hide pixels; they do not move them, so the mapping is the same. A pixel hidden by a crop or clip that lies under the mark is blanked too. For a right-angle rotation or a flip the blanked pixels are exactly those under the mark. For an oblique rotation or a skew, MuPDF blanks the mark's bounding box in the image's own axes, which is a few more pixels than the mark covers, never fewer (the decided, conservative limit). Pinned by `RedactionImageModel.QuarterTurnedImageLosesExactlyThePixelsUnderTheMark`, `.FlippedImageLosesExactlyThePixelsUnderTheMark`, `.CroppedImageLosesThePixelsUnderTheMark`, `.ClippedImageLosesThePixelsUnderTheMark` and `.ObliqueImageLosesAtLeastThePixelsUnderTheMark`. Each maps every stored pixel of the redacted image to the page through `pageCtm`, checks that the pixels under the mark are blank and those outside it kept, and checks the rendered page the same way. The rotation, flip and crop of the placement are kept.
- **Other placements of the same image.** Only the placement under the mark changes. Another `Do` of the same image on the page, the same image on another page, and a Form XObject shared with another page keep the original: MuPDF writes the redacted image as a new object for this placement, and copies a form per page before it filters it (`instance_forms`). The original pixels therefore stay in the file while another placement draws them. That is the intended behaviour: those placements are not under any mark, and they show the same pixels openly. Pinned by `.ImageDrawnTwiceOnlyTheMarkedPlacementChanges`, `.ImageSharedWithAnotherPageIsLeftOnTheOtherPage` and `.ImageInsideASharedFormIsRedactedOnlyOnTheMarkedPage`. To remove an image everywhere, mark every placement.
- **Not redacted** (unchanged by this phase, and the same in every method): images inside a tiling pattern, an ExtGState soft-mask group or a Type 3 glyph, because MuPDF's filter does not recurse into them. Inline images (`BI` ... `EI`) go through the same filter as image XObjects: under a partial mark the pixels under it are blanked in the image the page now draws and the rest is kept; under a full mark the inline image is removed; in both cases the original inline data (hex or binary) is not in the saved content. Pinned by `RedactionImageModel.InlineImageUnderAPartialMarkIsPixelRedacted` and `.InlineImageUnderAFullMarkIsRemoved`.
- **Full cover still removes.** `.FullyCoveredImageIsRemoved` checks that the image is no longer drawn and that no unredacted copy is in the saved file. The phase-1 probe `RedactionImageStateCorner.RedactedPixelsAreNotRecoverableFromTheSavedFile` checks that the saved file holds the partly redacted image and no object still carrying the original pixels under the mark.

### Per-mark methods (GitHub #441)

`RedactionData::imageMethod`, `textMethod` and `lineArtMethod` are now optional overrides. Unset (the default), a mark is applied with the `RedactionApplyOptions` method. Set, it is applied with its own:

- `restoreRedactionMark` stores the set ones on the mark's annotation dictionary (`/PDFEMarkOptions`), and `getRedactionMark(s)` reads them back. So they survive delete and undo (`DeleteRedactionCommand` keeps the `RedactionData` and restores it).
- `applyPageRedactions` works out each mark's methods. When all marks of a page have the same methods, the page is redacted in one `pdf_redact_page` call, as before. When they differ, each mark is applied on its own with `pdf_apply_redaction`, which filters the page for that one mark (text, line art, images and its black box) and removes it.
- **A method found in a file is ignored.** The stored dictionary carries a 128-bit token made once per process, and a method is honoured only when the token matches. Without that, a document could ship redaction marks that say "leave the image" (or the text) under a black box, and a user applying them would believe the content was gone. A mark made in another session is applied with the options' methods.
- **Text None keeps the text.** PDFE's residual page-content scrub (PDF-61) blanks every string in the page content that carries a word found under a mark. It now takes only the words under marks whose effective text method removes text, so a mark (or the options) with text method None leaves its text in place; before phase 5's claim-check remediation the scrub removed it anyway, so a text method of None had no effect while `scrubPageSurfaces` was on (the default). The other scrubs (overlapping annotations, document-scoped surfaces) still use every mark's words.

Pinned by `RedactionImageState.PerMarkImageMethodIsHonoured` (the phase-1 probe: None leaves its image, Remove removes its image), `RedactionImageModel.EveryPerMarkImageMethodIsHonoured` (None, Remove, Pixels, RemoveUnlessInvisible and an unset mark taking the options' method, on one page, read back before the apply), `.MarkMethodFoundInAFileIsIgnored` and `.PerMarkMethodSurvivesDeleteAndRestore`; for text and line art by `.EveryPerMarkTextMethodIsHonoured` (None keeps, Remove removes, unset takes the options' method, in both directions; read back, also after delete and restore), `.EveryPerMarkLineArtMethodIsHonoured` (None, RemoveIfTouched, and unset taking RemoveIfCovered for a covered and a touched path; read back) and `.TextAndLineArtMethodsFoundInAFileAreIgnored`.

### Flatten Form on one page (GitHub #442)

`FormManager::flattenForm(page)` used `pdf_bake_document(.., 0, 1)`, which bakes the widgets of every page. It now bakes only the given page (MuPDF's per-page baker is internal, so PDFE does the same steps): each widget's normal appearance becomes a Form XObject drawn at the widget's place in a content stream appended to the page, with the graphics state balanced first, and the widget is deleted from the page and from the AcroForm field tree (a parent field left with no kids is removed too). The page's `/Resources` and `/XObject` dictionaries are copied onto the page before a name is added, so a dictionary shared with another page, or inherited from the page tree, is never written to. The AcroForm itself is removed only when no field is left. Pinned by `FlattenImageState.FlattenFormFlattensOnlyTheGivenPage` and `FlattenImageModel.FlattenFormLeavesTheOtherPageUntouched` (page 1's annotations, resources and content are unchanged, with its own and with a shared `/Resources`; the flattened page looks as it did) and `.AcroFormIsRemovedWithTheLastField`, and the pruning of emptied parent fields by `.EmptiedParentFieldIsRemovedAndAParentWithKidsElsewhereIsKept` (a parent whose two widgets were on the flattened page is gone; a parent with a widget on the other page is kept with only that kid). `RedactionManager::flattenAnnotations` stays document-wide (NOT-A-DEFECT, see below).

### Flattened images (GitHub #437): selectable, not editable

Flattening bakes an annotation's appearance, so an image stamp or a signature image becomes an image drawn inside a Form XObject (the baked appearance). The phase-1 decision applies: images inside Form XObjects are enumerated and selectable, and NOT editable (see "Placements inside Form XObjects"). The baked appearance is a form the page draws once, but nothing marks it as unshared, and rewriting it would need the same per-page copy the editor refuses elsewhere. So a flattened image is listed by `enumerateImages` with `inFormXObject = true` and `occurrence = -1`, and the image tool can select it and shows its outline. As for any Form XObject placement, every image action and properties-panel control is disabled for it with the Form XObject tooltip (`ImagePlacementStateUiTest.PanelAndActionsAreDisabledForAFormPlacement`). Pinned by `FlattenImageState.FlattenedImageStampIsEnumerableByTheImageTool` (the phase-1 probe) and `FlattenImageModel.FlattenedImageStampIsSelectableButNotEditable` (listed at the stamp's place, `occurrence -1`; `ImageContentEditor::resizeImage` and `ImageManager::removePlacement` refuse it and the page is unchanged).

## OCR image replace and tagging

Phase 6 fixed the OCR text-replace write-back (`PdfContentWriter`, `ImageReplacer`, `RegionReconstructor`; GitHub #443-#447) and the content-stream tagger (`ContentStreamTagger::tagPageContent`, GitHub #438). The rules are pinned by `tests/test_tre_tagger_state.cpp` (the phase-1 probes) and `tests/test_tre_tagger_model.cpp`.

### Which image, and where (GitHub #443, #446)

`findImageXObjectForRegion`, `topmostImageOverRegion` and `findPageImageXObject` read the page's image draws with `ImagePlacementScanner`, the reader the editor and `ImageManager` use, Form XObjects included. Each result (`tre::PageImageInfo`) carries:

- `placementFitz`: the visible placement in the **rendered** frame (`pdf_page_transform`: box origin, `/Rotate`, Y down), the frame of `Page::render`, of a selection rect and of `ImageManager`'s `boundingBox`. It used to be the user-space bound flipped about the `fz_bound_page` height, which ignored the MediaBox origin and `/Rotate`;
- `occurrence` and `inFormXObject`: the placement's identity, as in "What a placement is";
- `pageCtm` (`hasPageCtm`): the full matrix, unit square to the rendered frame.

`findPageImageXObject` (the page's main image) considers only the page's own draws, and still leaves `placementFitz` empty.

Pinned by `TreReplaceStateTest.PlacementFrameFollowsMediaBoxOrigin`, `.PlacementFrameFollowsPageRotate` and `TreReplaceModelTest.PlacementFrameFollowsMediaBoxOriginAndRotate270Together`.

### Write-back through the full matrix (GitHub #444)

`ImageReplacer` composes the edit on a render of the page and writes the edited region back into the target image's own pixels. For an upright, unflipped placement the offset-and-scale arithmetic is unchanged. Any other placement goes through `PdfContentWriter::renderedFrameToImagePixels` (the inverse of `pageCtm`, then unit square to pixel rows): a quarter turn or a mirror is applied as an exact pixel remap and then resized with the same Lanczos/bilinear choice; an oblique or skewed placement is drawn through the matrix with smooth sampling. A crop changes nothing here: it hides pixels, it does not move them. When the target is the main-image fallback (no `placementFitz`), its matrix gives the placement instead of the old "the image is the page" assumption.

Pinned by `TreReplaceStateTest.WriteBackLandsWhereEditedOnRotatedImage`, `.WriteBackLandsWhereEditedOnFlippedImage` (with `.WriteBackLandsWhereEditedOnUprightImageControl` as the control) and `TreReplaceModelTest.WriteBackOnARotatedAndCroppedImageLandsInTheRect`, `.WriteBackOnAMirroredImageLandsInTheRect`, `.WriteBackOnARotatedAndFlippedImageLandsInTheRect` and `.WriteBackOnAnObliqueImageLandsInTheRect`. Each checks that the edited pixels are inside the edit rect (6 pt margin) and that the image outside it is unchanged.

**Size guard** (security-audit follow-up). A placement with a tiny scale is still invertible, and maps a page-sized edit to an enormous region of the image's pixel grid; a non-finite matrix maps it to NaN. Both paths (upright and through the matrix) check every value before converting it to int or allocating: it must be finite and well inside int range, and the region at most 16384 pixels a side and 100 million in all (or the target image's own size, when that is larger). An edit that fails is refused with "The edited region maps to an implausible size in the target image ..." and nothing is written. The checks live in `tre/WriteBackGuard.h`. Pinned by `TreReplaceSizeGuardTest.UprightPlacementWithATinyScaleIsRefused`, `.MirroredPlacementWithATinyScaleIsRefused` (a 1e-7 pt placement height), the control `.OrdinaryPlacementIsStillEdited`, and `TreReplaceSizeGuardUnitTest.NonFiniteAndHugeValuesAreRejected` (NaN and infinity, which the image finder already stops short of the write-back on a real page).

### What the new image keeps (GitHub #445)

`replacePageImage` writes the new pixels as a new image object and copies the old dictionary's non-pixel state onto it: `/SMask`, a stream `/Mask` (stencil), `/OC`, `/Intent` and `/Interpolate`. An ICCBased colour space with `/N 3` is kept, since the new samples are the same RGB numbers.

- `/Decode` is **not** copied, by decision: the pixels the replacer writes are decoded colours (what MuPDF draws), so copying it would invert them a second time. A `/Decode`-inverted image replaced with its own decoded pixels renders as before. This resolves the phase-1 NOT-PROBED row: `/Decode` loss was not a defect.
- A colour-key `/Mask` array names sample values. It is kept for 8-bit, 3-component samples without `/Decode`, which the new samples equal; otherwise it is dropped (decided limit).
- An ICCBased space with `/N 1` or `/N 4`, Indexed, Lab and the other spaces are written as DeviceRGB, because the new samples are RGB (decided limit). This build has no ICC engine (`FZ_ENABLE_ICC=0`), so it draws an ICCBased RGB image as DeviceRGB either way; the profile is kept for other viewers.
- A JPX image's own alpha (`/SMaskInData`) is not carried over: the replacer works on RGB pixels (decided limit).

Pinned by `TreReplaceStateTest.ReplaceKeepsTheImageSoftMask`, `TreReplaceModelTest.ReplaceKeepsSoftMaskIccOptionalContentAndRenderingIntent`, `.ReplaceKeepsStencilAndColourKeyMasksAndHonoursDecode` and `.ReplacingADecodeImageWritesNoDecodeAndShowsTheNewColours` (a source with `/Decode [1 0 1 0 1 0]` replaced with blue and green pixels: the replacement has no `/Decode` and shows blue and green, not yellow and magenta).

### One placement only (GitHub #446)

The new image replaces the old one **under its own name** only when nothing else uses that entry: the target is the only `Do` of that name that looks it up in the page's resources (page level, or inside a form that inherits them), and no other page shares the page's `/XObject` dictionary. Otherwise it is added under a fresh name (`PdfeTreIm1`, ...) and only the targeted `Do` is repointed at it, in place, with `ImagePlacementScanner::renameSite`: only that `Do`'s name operand is replaced, so its wrapper (`q`, `cm` and the numbers as written), the drawing order and every other byte of the content stay. Before claim-check round 2 the repoint used `rewriteSite`, which rewrote the whole wrapper: numbers were reformatted (`200.00` became `200`) and a `Do` outside a recognised wrapper got a new `q 1 0 0 1 0 0 cm ... Q` around it. The target is found by its occurrence, or by its placement when the caller gives none. A target that cannot be found while the entry is used elsewhere is refused. Undo restores the page content snapshot, which brings back the old `Do`.

A placement inside a Form XObject is refused (`replacePageImage` returns false and `ImageReplacer` reports that the image is inside a Form XObject): the phase-2 rule in "Placements inside Form XObjects". The occlusion check treats a form's image and a page image of the same name as different images.

Pinned by `TreReplaceStateTest.ReplaceTargetsOnlyTheEditedPlacement` and `TreReplaceModelTest.SolePlacementIsReplacedUnderItsOwnNameWithoutTouchingTheContent`, `.AnImageInADictSharedWithAnotherPageChangesOnlyOnThisPage`, `.RepointedPlacementIsUndoneByTheContentSnapshot`, `.RepointChangesOnlyTheTargetedDoNameInTheContent` (byte comparison of the page content before and after two repoints: the only difference is the targeted name) and `.AnImageInsideAFormIsReportedButNotWritten`.

Performance note (an implementation detail, not a behaviour; no test pins it, and the outcome is the same either way): checking whether another page shares the `/XObject` dictionary costs one resources lookup per page, O(pages). The code is written to skip the check when another placement on this page already uses the entry, and to return at the first sharing page.

### A picture of the whole page (GitHub #447)

The two-argument `replacePageImage(page, document, picture)` takes a picture of the whole page (what `RegionReconstructor` passes). When the page's main image is upright and covers the page (each edge within 1 pt or 0.5%), the picture is the image and it is written as before. Otherwise only the part of the picture under the image is taken, through the full matrix, and drawn into the image's own pixel grid on top of its current pixels. A whole-page render is never squeezed into a smaller image, and a scan on a `/Rotate 90` page is written the right way round.

Pinned by `TreReplaceStateTest.WholePageRenderIsNotWrittenIntoASmallerImage`, `TreReplaceModelTest.WholePagePictureOfARotatedScanIsWrittenTheRightWayRound` and the control `.WholePagePictureOfAnUprightScanIsTheImage`.

### Tagging page content (GitHub #438)

`tagPageContent` reads the page content as a list of operators (each with its operands, across every stream of `/Contents`; an inline image counts as one) and wraps each region in its own `BDC ... EMC`. It used to wrap the whole stream in the first region's tag and append the others empty.

- **With operator ranges** (`startOperatorIndex`, `endOperatorIndex`, end exclusive), each region encloses exactly those operators. A range that would cut a `q`/`Q`, `BT`/`ET` or `BDC`/`EMC` pair, or that overlaps an earlier region, is not written (a warning is logged) and the region is emitted empty, so the stream stays well formed. Pinned by `TaggerModelTest.ARangeThatCutsANestedPairIsNotWritten` (`q`/`Q`), `.ARangeThatCutsABtEtPairIsNotWritten`, `.ARangeThatCutsABdcEmcPairIsNotWritten` and `.AnExplicitRangeOverlappingAnEarlierRegionIsNotWritten` (the earlier region keeps exactly its operators; the overlapping one gets only its empty sequence at the end).
- **Without ranges** (what `PdfUAConverter` and `ComplianceManager` pass, since `AutoTagger` knows a block's kind and order but not its operators), the tagger places the regions itself, in content order. Each image draw (an image `Do` together with a placement wrapper that holds nothing else, or an inline image) goes to the next `/Figure` region. The text objects and Form XObject draws go to the other regions, as evenly as the nesting allows, and a text region never encloses an image draw. Content already inside a marked-content sequence (an existing tag or an artifact) is left as it is and never nested into.
- A region with nothing of its own (more Figure regions than images, for example) still gets its sequence, empty, at the end, so its MCID exists and points at nothing rather than at the page.
- Limits (decided): with more separable text runs than text regions, the extra runs stay untagged; with more text regions than runs, the extra regions are empty. Vector graphics outside any text or image unit are not tagged.
- An inline image is lexed from `BI` through its dictionary to the `ID` operator token, then to an `EI` that stands alone between white space. A key or value that merely contains the letters ID (`/IDx`, a string) is not the operator.
- Cost (security-audit follow-up): the opener/closer pairs are matched in one stack pass and indexed in a segment tree, so growing a span to its balanced hull costs O(log n) a step instead of a rescan, and the last text block's hull is grown rather than recomputed. The automatic placement is near-linear on deep or many nested `q`/`Q`, `BT`/`ET` and `BDC`/`EMC` pairs (20000 nested pairs with 20000 text objects and 20000 images: well under a second). A firm budget of growth steps per page (16 per operator, plus 4096) bounds a hostile stream whose pairs cross (`q BT Q ET`, chained): past it the page's content is left untagged, a warning is logged, and each region gets its empty sequence. The output for well-formed content is unchanged (checked against the previous algorithm on 200000 generated streams). Pinned by `TaggerModelTest.DeeplyNestedContentIsPlacedInBoundedTime`, `.CrossingPairChainHitsTheBudgetAndIsLeftUntagged`, `.InlineImageDictionaryWithIdLookalikesIsOneOperator` and `.InlineImageDictionaryWithIdLookalikesIsPlacedWhole`.

Tagging changes no pixel, and the existing structure tree and its MCIDs are untouched. Pinned by `TaggerStateTest.FigureMcidEnclosesTheImageDo`, `.TextMcidDoesNotEncloseTheImageDo` and `TaggerModelTest.ExplicitRangesWrapEachRegionInItsOwnSequence`, `.WithoutRangesEachFigureGetsItsImageAndTextNeverEnclosesOne`, `.AutoTaggerRegionsPointFiguresAtTheImages` (the production AutoTagger regions), `.ExistingMarkedContentIsKeptAndNotNestedInto`, `.ARangeThatCutsANestedPairIsNotWritten`, `.TaggingKeepsTheRendering` and `.ExistingStructureTreeIsPreserved`.

## Inventory

### Every writer and deriver, against every dimension

This table records the code as audited on 2026-11-17, before the fixes. Columns:

- **rot**: rotation
- **clip**: a clip or crop around the placement
- **flip/skew**: flips and skew in the CTM
- **gs**: an ExtGState wrapper (opacity, soft mask)
- **order/tag/OC**: stacking order, marked content (MCID) and optional content
- **copies**: the other placements of the same image
- **form**: images drawn inside a Form XObject
- **/Rotate**: the page's rotation and MediaBox origin
- **undo**: whether undo restores the state
- **UI**: whether the UI shows the page's state and its controls work

Cell values:

- **ok**: state is preserved, or the path is correct.
- **#nnn**: defect, tracked by that GitHub issue.
- **–**: not applicable.

| function (file) | rot | clip | flip/skew | gs | order/tag/OC | copies | form | /Rotate | undo | UI |
|---|---|---|---|---|---|---|---|---|---|---|
| `ImageContentEditor::resizeImage` (move, resize) | ok (GitHub #416) | #419 | #424 | #428 | #429, #430 | #421 | #427 | #426 | ok | – |
| `ImageContentEditor::rotateImage` | ok | #419 | #424 | #428 | #429, #430 | #421 | #427 | #426 | ok | – |
| `ImageContentEditor::cropImage` | #417 | #417 | #424 | #428 | #429, #430 | #421 | #427 | #426 | ok | – |
| `ImageContentEditor` `removeFromContentStream` | – | drops it with the `Do` (#419) | – | drops it with the `Do` (#428) | – | #421 | #427 | – | – | – |
| `ImageManager::enumerateImages` | ok | #420 | #424 | – | – | ok (one entry per `Do`) | #427 | #426 | – | – |
| `ImageManager::insertImage` | – | – | – | – | appended last (new) | – | – | ok | #434 | #435, #436 |
| `ImageManager::restoreImage` (undo) | ok | ok | ok | ok (resource kept) | ok | ok | – | – | ok | – |
| `ImageManager::extractImage` | – | – | – | mask not applied (decided, see below) | – | – | – | – | – | ok (GitHub #439) |
| `ImageCommands` (Insert, Resize, Rotate, Crop) | ok | – | – | – | – | by name (#421) | – | – | Insert undo #434 | – |
| `ImageTool` (drag, handles, re-sync) | ok (GitHub #416) | #420 | – | – | – | selects first match (#421) | – | #426 | – | handle axis #433 |
| `MainWindow` image actions | – | – | – | – | – | – | – | – | – | #422, #431, #432 |
| `ImagePropertiesPanel` | – | – | – | – | – | – | – | – | – | #423, #425 |
| `BatchEngine` image watermark | ok (GitHub #451) | – | – | ok (GitHub #448, #449) | – | – | – | ok (GitHub #450) | – | ok (GitHub #451) |
| `ExifStripper` | ok | ok | ok | ok: masks kept (GitHub #452) | ok | touches only images that carry metadata | – | – | – | – |
| redaction (`applyPageRedactions`) | ok (GitHub #440) | ok (GitHub #440) | ok (GitHub #440) | – | – | ok (GitHub #440) | ok (GitHub #440) | – | – | ok (GitHub #440, #441) |
| `FormManager::flattenForm` | – | – | – | – | – | – | – | – | – | ok: page scope (GitHub #442) |
| flatten output (baked stamps) | – | – | – | – | – | – | ok: listed, not editable (GitHub #437) | – | – | – |
| TRE `PdfContentWriter::replacePageImage` | #444 | – | #444 | mask/ICC #445 | – | #446 | – | #443 | – | – |
| TRE `RegionReconstructor` | – | – | – | – | – | – | – | – | – | #447 |
| `ContentStreamTagger::tagPageContent` | – | – | – | – | #438 | – | – | – | – | – |

**After phase 2**, the `ImageContentEditor` rows (#416, #417, #419, #421, #424, #426, #427, #428, #429, #430) and the `ImageManager::enumerateImages` row (#420, #424, #426, #427) are fixed, as described above. The form column is fixed for enumeration; editing a form placement is a decided limit (see "Placements inside Form XObjects"). The `removeFromContentStream` row no longer applies: removal cuts one placement in place. The `ImageCommands` "by name" cell is fixed: commands, editor and `ImageManager::removePlacement` take name + occurrence. MainWindow's resize, rotate and crop handlers pass the selected placement's occurrence. The rest of the UI work (relative rotate, properties panel, handle axes) is phase 3.

**After phase 3**, the remaining editor and UI rows are fixed:
- the `ImageManager::insertImage` row: `jpegQuality` (#435), EXIF orientation (#436) and metadata, and the identity it reports;
- the `ImageCommands` Insert undo row (#434);
- the `ImageTool` handle-axis row (#433);
- the `MainWindow` row (#422, #431, #432);
- the `ImagePropertiesPanel` row (#423, #425).

See "Image actions in the UI" and "Inserting an image".

**After phase 5**, the redaction, Flatten Form and flatten-output rows are fixed: redaction uses Pixels mode and maps the mark through the full placement (#440), honours each mark's own methods (#441), Flatten Form bakes only its page (#442), and a flattened image is listed and selectable but not editable (#437, the decided Form XObject limit). See "Redaction and flatten". The metadata stripper also gained the two bounds the phase-4 security audit asked for (frame-header search past `FF DA` and behind a buried DCT filter; a visited set for masks and `/Alternates`).

**After phase 6**, the OCR replace rows (`PdfContentWriter::replacePageImage`: #443, #444, #445, #446; `RegionReconstructor`: #447) and the `ContentStreamTagger::tagPageContent` row (#438) are fixed. See "OCR image replace and tagging". Every row of the table is now fixed or a decided limit.

**After phase 4**, the watermark, metadata-stripping and extraction rows are fixed: the `BatchEngine` image watermark row (#448, #449, #450, #451; the text watermark gets the same resource and placement rules), the `ExifStripper` row (#452, and #453, which has no column: CMYK colours) and the `ImageManager::extractImage` row (#439). See "Watermark, metadata stripping and extraction". Extracting an image's soft mask was decided in phase 6 (below).

### Every finding and its status

There are four statuses:

- **CONFIRMED**: a probe test asserts the correct behaviour and fails on the code before the fix.
- **NOT-A-DEFECT**: investigated, and the behaviour is correct.
- **NOT-PROBED**: no observable to assert on, with the reason and how to reproduce it.
- **CONTROL**: a test that passes by design, to show the oracle works.

Each CONFIRMED probe fails before the fix and passes after it.

| area | finding | issue | fixed in phase | probe |
|---|---|---|---|---|
| image editor | crop reset rotation; clipped in page axes | GitHub #417 | 2 | `ImagePlacementStateTest.CropKeepsRotation`, `.CropOfRotatedImageClipsInTheImagesAxes` |
| image editor | move/rotate dropped a crop | GitHub #419 | 2 | `ImagePlacementStateTest.MoveKeepsCrop`, `.RotateKeepsCrop` |
| image editor | outline of a cropped image was the uncropped image | GitHub #420 | 2 | `ImagePlacementStateTest.OutlineOfACroppedImageIsTheVisibleRegion` |
| image editor | editing one copy rewrote every copy | GitHub #421 | 2 | `ImagePlacementStateTest.EditingOnePlacementLeavesTheOtherCopy` |
| image editor | an edit dropped flips and skew | GitHub #424 | 2 | `ImageEditorStateTest.E1_*` |
| image editor | an edit dropped an opacity (`gs`) wrapper | GitHub #428 | 2 | `ImageEditorStateTest.E2_OpacityWrapperSurvivesMove` |
| image editor | an edit moved the image above later content | GitHub #429 | 2 | `ImageEditorStateTest.E3_ImageStaysBeneathLaterContentAfterEdit` |
| image editor | an edit dropped marked content (MCID) and layer (OC) | GitHub #430 | 2 | `ImageEditorStateTest.E4_*` |
| image editor | enumeration ignored page /Rotate | GitHub #426 | 2 | `ImageEditorStateTest.E12_EnumeratedBoxMatchesRenderOnRotatedPage` |
| image editor | images inside Form XObjects not enumerated | GitHub #427 | 2 | `ImageEditorStateTest.E13_ImageInsideFormXObjectIsEnumerated` |
| image UI | "Rotate Image..." set an absolute 90° | GitHub #422 | 3 | `ImagePlacementStateWindowTest.RotateImageActionRotatesFurtherEachTime` |
| image UI | properties panel controls did nothing; showed 0° | GitHub #423 | 3 | `ImagePlacementStateWindowTest.PropertiesPanel*` |
| image UI | crop did not re-sync the image tool | GitHub #431 | 3 | `ImageEditorStateWindowTest.E5_CropResyncsTheImageTool` |
| image UI | rotate 180/270 unreachable; rotate not relative | GitHub #432 | 3 | `ImageEditorStateWindowTest.E6_*` |
| image UI | handle-resize of a rotated image used page axes | GitHub #433 | 3 | `ImageEditorStateTest.E7_RightHandleDragOfRotatedImageGrowsItsOwnWidth` |
| image UI | undo of an insert left the XObject resource | GitHub #434 | 3 | `ImageEditorStateTest.E8_InsertUndoRemovesImageAndItsResource` |
| image UI | insert ignored JPEG quality | GitHub #435 | 3 | `ImageEditorStateTest.E9_JpegQualityChangesTheEncodedImage` |
| image UI | insert ignored EXIF orientation | GitHub #436 | 3 | `ImageEditorStateTest.E9_ExifOrientationIsApplied` |
| image UI | JPX and JBIG2 reported as Lossless | GitHub #425 | 3 | `ImageEditorStateDataTest.E10_JpxAndJbig2AreNotReportedAsLossless` |
| watermark | overwrote the page's own /GS0 | GitHub #448 | 4 | `WatermarkState.W1_ImageWatermarkKeepsPagesOwnGS0` |
| watermark | shadowed inherited resources; reused the page's /F1 | GitHub #449 | 4 | `WatermarkState.W2_*` |
| watermark | image watermark off-centre on a rotated page | GitHub #450 | 4 | `WatermarkState.W3_ImageWatermarkIsCentredOnRotatedPage` |
| watermark | image watermark ignored rotation and position | GitHub #451 | 4 | `WatermarkState.W4_*` |
| EXIF stripping | dropped soft and stencil masks | GitHub #452 | 4 | `ExifState.X1_*` |
| EXIF stripping | corrupted CMYK images | GitHub #453 | 4 | `ExifState.X2_CmykColoursSurviveStrip` |
| extraction | CMYK extracted with wrong colours | GitHub #439 | 4 | `ExtractState.R1_ExtractedCmykImageMatchesRenderedColour` |
| redaction | a partial overlap deleted the whole image | GitHub #440 | 5 | `RedactionImageStateCorner.*` |
| redaction | per-mark image method ignored | GitHub #441 | 5 | `RedactionImageState.PerMarkImageMethodIsHonoured` |
| forms | flattening one page's form flattened every page | GitHub #442 | 5 | `FlattenImageState.FlattenFormFlattensOnlyTheGivenPage` |
| flatten | a flattened image stamp was not enumerable | GitHub #437 | 2 (enumeration); 5 (decided: listed, not editable) | `FlattenImageState.FlattenedImageStampIsEnumerableByTheImageTool`, `FlattenImageModel.FlattenedImageStampIsSelectableButNotEditable` |
| OCR image replace | placement frame ignored MediaBox origin and /Rotate | GitHub #443 | 6 | `TreReplaceStateTest.PlacementFrameFollows*` |
| OCR image replace | write-back assumed an upright, unflipped image | GitHub #444 | 6 | `TreReplaceStateTest.WriteBackLandsWhereEditedOn*` |
| OCR image replace | replace dropped the soft mask | GitHub #445 | 6 | `TreReplaceStateTest.ReplaceKeepsTheImageSoftMask` |
| OCR image replace | replace rewrote every placement | GitHub #446 | 6 | `TreReplaceStateTest.ReplaceTargetsOnlyTheEditedPlacement` |
| OCR image replace | whole-page render written into a smaller image | GitHub #447 | 6 | `TreReplaceStateTest.WholePageRenderIsNotWrittenIntoASmallerImage` |
| tagging | whole stream wrapped in the first region's tag | GitHub #438 | 6 | `TaggerStateTest.*` |

All rows above are **CONFIRMED**. The remaining audit items are:

| item | status | reason |
|---|---|---|
| undo of an edit restores an ExtGState resource | NOT-A-DEFECT, pinned as CONTROL in phase 2 | A trial probe (move then undo an image wrapped in `gs`) was green: alpha was back to about 50% and the ExtGState was still in the saved page. The opacity loss on an edit (#428) is the `gs` operator leaving the content stream, not the resource leaving the page. Pinned by `ImagePlacementStateUndoTest.UndoOfAnEditRestoresTranslucencyAndExtGState` (passes by design). The restore mechanism itself is pinned by `.RestoreImagePutsBackTheExtGStateResource`, and exact-byte undo/redo of a crop by `.UndoAndRedoOfCropRestoreTheExactContentBytes`. |
| batch annotation flatten (`RedactionManager::flattenAnnotations`) flattens every page | NOT-A-DEFECT | It is documented as document-wide and has no page parameter. Its only caller (`BatchEngine`) works on whole documents. |
| `stripExif=false` on insert | NOT-A-DEFECT, pinned in phase 3 | Insert always decodes and re-encodes the pixels from a fresh image, so EXIF is never copied and stripping always happens. Keeping metadata has no defined result in a PDF image XObject. The dialog's checkbox is therefore always on and disabled, with a tooltip. `ImagePlacementStateInsertTest.JpegSourceIsStoredAsDctWithNoMetadata` inserts a JPEG carrying an APP1 Exif segment and a comment, with `stripExif` true and false, and checks that neither the stream, nor a `Metadata` entry, nor the saved file holds them. |
| EXIF stripper rewrites non-JPEG images | NOT-PROBED in phase 1; decided and pinned in phase 4 | In phase 1 `ExifStripper.h` documented a QImage round trip of every image, so "non-JPEG untouched" was not then the intended behaviour. Phase 4 replaced the round trip (it caused #452 and #453): an image with nothing to strip is now left byte for byte as it was. Pinned by `ExifStripModel.ImagesWithNothingToStripAreUntouched`. |
| Mask, Decode, ICC and OC lost by OCR replace | fixed in phase 6 with #445 (Mask, ICC, OC); Decode NOT-A-DEFECT | A stream `/Mask`, `/OC` and an ICCBased RGB colour space are copied onto the new image, and a colour-key `/Mask` for 8-bit RGB samples (`TreReplaceModelTest.ReplaceKeepsSoftMaskIccOptionalContentAndRenderingIntent`, `.ReplaceKeepsStencilAndColourKeyMasksAndHonoursDecode`). `/Decode` is correctly not copied: the written pixels are already decoded colours, and a `[1 0 1 0 1 0]` image replaced with its own decoded pixels renders unchanged (the same test). |
| extracting an image ignores its soft mask | NOT-PROBED in phase 1; decided NOT-A-DEFECT in phase 6 | `ImageManager::extractImage` returns the image XObject's own samples, as stored and decoded. A `/SMask` is a separate image XObject and is not applied, so an extracted soft-masked image comes out opaque; that is the decided behaviour (an extract is the image's pixels, not the page's composite). Pinned by `ImageExtractDecisionTest.SoftMaskedImageExtractsItsOwnSamplesWithoutTheMask`. Tracked with GitHub #439, the same function's colour handling. |
| OCR write-back on an upright image | CONTROL | `TreReplaceStateTest.WriteBackLandsWhereEditedOnUprightImageControl` passes by design, so a T2 failure can only be the rotation or flip handling. |
