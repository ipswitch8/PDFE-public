#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tre/TreTypes.h"

// Forward-declare pugixml types to avoid including pugixml.hpp in the header.
namespace pugi {
class xml_node;
}

namespace pdfe::tre {

/// Parses Tesseract HOCR XML output into structured document data and
/// supports extracting text from pixel-coordinate regions with
/// snap-to-line-boundary semantics.
class HocrParser {
public:
    /// Parse a complete HOCR XML string into a structured HocrDocument.
    ///
    /// The input should be a valid HOCR/HTML document produced by Tesseract.
    /// If the XML is malformed or contains no recognizable page element,
    /// an empty HocrDocument is returned with default values.
    [[nodiscard]] static HocrDocument parse(std::string_view hocrXml);

    /// Extract text data for a specific region given in pixel coordinates.
    ///
    /// Uses snap-to-line-boundary semantics: any line whose bounding box
    /// overlaps the selection rectangle is included in its entirety.
    /// The resulting RegionTextData has its pixelBbox set to the union
    /// of all included line bounding boxes, and pdfBbox left empty
    /// (the caller is responsible for coordinate conversion via CoordMapper).
    [[nodiscard]] static RegionTextData extractRegion(
        const HocrDocument& doc,
        const PixelRect& selectionPixels
    );

private:
    /// Parsed properties from an HOCR element's title attribute.
    ///
    /// Title format example:
    ///   "bbox 150 208 280 236; x_wconf 95; x_fsize 14; baseline 0.002 -7;
    ///    x_size 28.5; x_descenders 5.5; x_ascenders 7; scan_res 300 300"
    struct TitleProps {
        PixelRect bbox;
        int confidence = -1;          ///< x_wconf (word confidence, 0-100)
        float fontSizePx = 0.0f;      ///< x_fsize
        float baselineSlope = 0.0f;   ///< baseline slope component
        float baselineOffset = 0.0f;  ///< baseline offset component
        float xSizePx = 0.0f;         ///< x_size (row height in pixels, ≈ em-square)
        float descendersPx = 0.0f;    ///< x_descenders
        float ascendersPx = 0.0f;     ///< x_ascenders
        int scanResX = 0;             ///< scan_res horizontal DPI
        int scanResY = 0;             ///< scan_res vertical DPI
        bool hasBbox = false;         ///< Whether a bbox was successfully parsed
        /// Per-character bounding boxes from x_bboxes (word-level property).
        /// Tesseract packs all char bboxes as a flat list of x0 y0 x1 y1
        /// values in the word title: "x_bboxes x0 y0 x1 y1 x0 y0 x1 y1 ...".
        std::vector<PixelRect> charBboxes;
    };

    /// Parse the semicolon-delimited title attribute from an HOCR element.
    [[nodiscard]] static TitleProps parseTitleAttribute(std::string_view title);

    /// Parse an ocr_carea block element and its children.
    [[nodiscard]] static HocrBlock parseBlock(const pugi::xml_node& blockNode);

    /// Parse an ocr_par paragraph element and its children.
    [[nodiscard]] static HocrParagraph parseParagraph(const pugi::xml_node& parNode);

    /// Parse an ocr_line span element and its children.
    [[nodiscard]] static HocrLine parseLine(const pugi::xml_node& lineNode);

    /// Parse an ocrx_word span element, extracting text and properties.
    [[nodiscard]] static HocrWord parseWord(const pugi::xml_node& wordNode);
};

} // namespace pdfe::tre
