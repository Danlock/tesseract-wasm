#include <emscripten/bind.h>
#include <emscripten/emscripten.h>
#include <leptonica/allheaders.h>
#include <tesseract/baseapi.h>
#include <tesseract/ocrclass.h>

#include <format>
#include <memory>
#include <string>
#include <vector>

struct IntRect {
  int left = 0;
  int right = 0;
  int top = 0;
  int bottom = 0;
};

enum LayoutFlag {
  StartOfLine = 1,
  EndOfLine = 2,
};

using LayoutFlags = int;

struct TextRect {
  IntRect rect;
  LayoutFlags flags = 0;
  float confidence = 0.0f;
  std::string text;
};

struct Orientation {
  int rotation = 0;
  float confidence = 0.0f;
};

struct GetVariableResult {
  bool success;
  std::string value;
};

enum class TextUnit {
  Word,
  Line,
};

template <class T>
std::unique_ptr<T> unique_from_raw(T* ptr) {
  return std::unique_ptr<T>(ptr);
}

std::string string_from_raw(char* ptr) {
  auto result = std::string(ptr);
  delete[] ptr;
  return result;
}

auto iterator_level_from_unit(TextUnit unit) {
  tesseract::PageIteratorLevel level;
  if (unit == TextUnit::Line) {
    return tesseract::RIL_TEXTLINE;
  } else if (unit == TextUnit::Word) {
    return tesseract::RIL_WORD;
  } else {
    return tesseract::RIL_SYMBOL;
  }
}

typedef std::string OCRResult;

using namespace emscripten;

class ProgressMonitor : public tesseract::ETEXT_DESC {
 public:
  ProgressMonitor(const emscripten::val& callback) : js_callback_(callback) {
    progress_callback2 = progress_handler;
  }

  void ProgressChanged(int percentage) {
    if (!js_callback_.isUndefined()) {
      js_callback_(percentage);
    }
  }

 private:
  static bool progress_handler(tesseract::ETEXT_DESC* monitor, int left,
                               int right, int top, int bottom) {
    static_cast<ProgressMonitor*>(monitor)->ProgressChanged(monitor->progress);
    return true;
  }
  emscripten::val js_callback_;
};

/**
 * ByteView mallocs some bytes and exposes the memory via emscripten::typed_memory_view.
 */
class ByteView {
 public:
  ByteView(size_t size) {
    size_ = size;
    bytes_ = (const unsigned char *) malloc(size);
  }

  ~ByteView() { free((void *) bytes_); }

  emscripten::val Data() const {
    return emscripten::val(emscripten::typed_memory_view(size_, bytes_));
  }

  size_t Size() const { return size_; }
  const unsigned char * Bytes() const { return bytes_; }

 private:
  size_t size_;
  const unsigned char *bytes_;
};

class OCREngine {
 public:
  OCREngine() : tesseract_(new tesseract::TessBaseAPI()) {}

  ~OCREngine() { tesseract_->End(); }

  std::string Version() const { return tesseract_->Version(); }

  OCRResult LoadModel(const ByteView& model, const std::string& lang) {
    auto result = tesseract_->Init(
        (const char *) model.Bytes(), model.Size(), lang.c_str(), tesseract::OEM_LSTM_ONLY,
        nullptr /* configs */, 0 /* configs_size */, nullptr /* vars_vec */,
        nullptr /* vars_values */, false /* set_only_non_debug_params */, nullptr /* reader */
    );
    if (result != 0) {
      return OCRResult("Failed to load training data");
    }
    return {};
  }

  GetVariableResult GetVariable(const std::string& var_name) const {
    auto name = var_name.c_str();
    std::string val;
    bool success = tesseract_->GetVariableAsString(name, &val);
    if (!success) {
      return {.success = false};
    }
    return {.success = true, .value = val};
  }

  OCRResult SetVariable(const std::string& var_name,
                        const std::string& var_value) {
    auto name = var_name.c_str();
    auto value = var_value.c_str();
    bool success = tesseract_->SetVariable(name, value);
    if (!success) {
      return OCRResult("Failed to set value for variable " + var_name);
    }

    return {};
  }

  OCRResult LoadImage(const ByteView& view) {
    // Unavoidable copy of our ByteView into a Leptonica Pix.
    // Using pixGetData() instead like robert-knight/tesseract-wasm originally did is another option,
    // but then Go would need to re encode images to Leptonica's Pix format in memory anyway.
    auto pix = pixReadMem(view.Bytes(), view.Size());
    if (pix == nullptr) {
      return OCRResult("pixReadMem failed");
    }

    // Initialize for layout analysis only if a model has not been loaded.
    // This is a no-op if a model has been loaded.
    tesseract_->InitForAnalysePage();
    // Tesseract SetImage also copies the Pix for internal use, unfortunately.
    // Doesn't seem like I can get rid of that without adding Tesseract patches. Possibly worth it...
    tesseract_->SetImage(pix);

    layout_analysis_done_ = false;
    ocr_done_ = false;
    // Tesseract copies the Pix internally, so we should clean up immediately.
    pixDestroy(&pix);
    return {};
  }

  void ClearImage() {
    tesseract_->Clear();
    layout_analysis_done_ = false;
    ocr_done_ = false;
  }

  std::vector<TextRect> GetBoundingBoxes(TextUnit unit) {
    if (!layout_analysis_done_) {
      tesseract_->AnalyseLayout();
      layout_analysis_done_ = true;
    }
    return GetBoxes(unit, false /* with_text */);
  }

  std::vector<TextRect> GetTextBoxes(TextUnit unit,
                                     const emscripten::val& progress_callback) {
    DoOCR(progress_callback);
    return GetBoxes(unit, true /* with_text */);
  }

  std::string GetText(const emscripten::val& progress_callback) {
    DoOCR(progress_callback);
    return string_from_raw(tesseract_->GetUTF8Text());
  }

  std::string GetHOCR(const emscripten::val& progress_callback) {
    DoOCR(progress_callback);
    auto hocr_body = string_from_raw(tesseract_->GetHOCRText(0));

    // The header and footer of the hOCR document are taken from
    // `TessHOcrRenderer::BeginDocumentHandler` and
    // `TessHOcrRenderer::EndDocumentHandler` respectively. We can't use that
    // class directly because it expects to write to a file.
    auto hocr_doc = std::format(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" "http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head>
  <title>hOCR text</title>
  <meta http-equiv="Content-Type" content="text/html;charset=utf-8"/>
  <meta name='ocr-system' content='tesseract {}' />
  <meta name='ocr-capabilities' content='ocr_page ocr_carea ocr_par ocr_line ocrx_word ocrp_wconf' />
</head>
<body>
  {}
</body>
</html>)",
                                tesseract_->Version(), hocr_body);

    return hocr_doc;
  }

  Orientation GetOrientation() {
    // Tesseract's orientation detection is part of the legacy (non-LSTM)
    // engine, which is not compiled in to reduce binary size. Hence we use
    // Leptonica's orientation detection instead. See comments for
    // `pixOrientDetect` in the Leptonica source for details of how it works.
    //
    // The method is simplistic, and is designed for latin text, but it serves
    // as a baseline that can be improved upon later.
    auto pix = tesseract_->GetThresholdedImage();

    // Metric that indicates whether the image is right-side up vs upside down.
    // +ve indicates right-side up.
    float up_conf = 0;

    // Metric that indicates whether the image is right-side up after being
    // rotated 90 degrees clockwise.
    float left_conf = 0;

    auto had_error = pixOrientDetect(pix, &up_conf, &left_conf,
                                     0 /* min_count */, 0 /* debug */);
    pixDestroy(&pix);

    if (had_error) {
      // If there is an error, we currently report a result with zero confidence
      // score.
      return {};
    }

    // Are we more confident that the image is rotated at 0/180 degrees than
    // 90/270?
    auto is_up_or_down = abs(up_conf) - abs(left_conf) > 5.0;
    int rotation;
    if (is_up_or_down) {
      if (up_conf > 0) {
        rotation = 0;
      } else {
        rotation = 180;
      }
    } else {
      if (left_conf < 0) {
        rotation = 90;
      } else {
        rotation = 270;
      }
    }
    return {.rotation = rotation, .confidence = 1};
  }

 private:
  std::vector<TextRect> GetBoxes(TextUnit unit, bool with_text) {
    auto iter = unique_from_raw(tesseract_->GetIterator());
    if (!iter) {
      return {};
    }

    auto level = iterator_level_from_unit(unit);
    std::vector<TextRect> boxes;
    do {
      TextRect tr;
      if (with_text) {
        // Tesseract provides confidence as a percentage. Convert it to a score
        // in [0, 1]
        tr.confidence = iter->Confidence(level) * 0.01;
        tr.text = string_from_raw(iter->GetUTF8Text(level));
      }

      if (unit < TextUnit::Line) {
        if (iter->IsAtBeginningOf(tesseract::RIL_TEXTLINE)) {
          tr.flags |= LayoutFlag::StartOfLine;
        }
        if (iter->IsAtFinalElement(tesseract::RIL_TEXTLINE, level)) {
          tr.flags |= LayoutFlag::EndOfLine;
        }
      }

      iter->BoundingBox(level, &tr.rect.left, &tr.rect.top, &tr.rect.right,
                        &tr.rect.bottom);
      boxes.push_back(tr);
    } while (iter->Next(level));

    return boxes;
  }

  void DoOCR(const emscripten::val& progress_callback) {
    ProgressMonitor monitor(progress_callback);
    if (!ocr_done_) {
      tesseract_->Recognize(&monitor);
      layout_analysis_done_ = true;
      ocr_done_ = true;
    }
    // Tesseract doesn't seem to report 100% progress in `Recognize`, and
    // won't have reported progress if OCR has already been done, so report
    // completion ourselves.
    monitor.ProgressChanged(100);
  }

  bool layout_analysis_done_ = false;
  bool ocr_done_ = false;
  std::unique_ptr<tesseract::TessBaseAPI> tesseract_;
};

EMSCRIPTEN_BINDINGS(ocrlib) {
  value_object<IntRect>("IntRect")
      .field("left", &IntRect::left)
      .field("top", &IntRect::top)
      .field("right", &IntRect::right)
      .field("bottom", &IntRect::bottom);

  value_object<TextRect>("TextRect")
      .field("rect", &TextRect::rect)
      .field("flags", &TextRect::flags)
      .field("confidence", &TextRect::confidence)
      .field("text", &TextRect::text);

  value_object<Orientation>("Orientation")
      .field("rotation", &Orientation::rotation)
      .field("confidence", &Orientation::confidence);

  value_object<GetVariableResult>("GetVariableResult")
      .field("success", &GetVariableResult::success)
      .field("value", &GetVariableResult::value);

  class_<ByteView>("ByteView")
      .constructor<size_t>()
      .function("data", &ByteView::Data);

  class_<OCREngine>("OCREngine")
      .constructor<>()
      .function("clearImage", &OCREngine::ClearImage)
      .function("getBoundingBoxes", &OCREngine::GetBoundingBoxes)
      .function("getHOCR", &OCREngine::GetHOCR)
      .function("getOrientation", &OCREngine::GetOrientation)
      .function("getText", &OCREngine::GetText)
      .function("getTextBoxes", &OCREngine::GetTextBoxes)
      .function("getVariable", &OCREngine::GetVariable)
      .function("loadImage", &OCREngine::LoadImage)
      .function("loadModel", &OCREngine::LoadModel)
      .function("setVariable", &OCREngine::SetVariable);

  enum_<TextUnit>("TextUnit")
      .value("Line", TextUnit::Line)
      .value("Word", TextUnit::Word);

  register_vector<IntRect>("vector<IntRect>");
  register_vector<TextRect>("vector<TextRect>");
}
