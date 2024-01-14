#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
//#include <boost/algorithm/string.hpp>
#include <utf8proc.h>
#include <memory>
#include <sstream>

//#include <onnxruntime/core/session/onnxruntime_cxx_api.h>

#include <onnxruntime_cxx_api.h>

//https://unicode.org/reports/tr15/#Norm_Forms
//https://ssl.icu-project.org/apiref/icu4c/uchar_8h.html

const std::wstring stripChar = L" \t\n\r\v\f";
using Vocab = std::unordered_map<std::wstring, size_t>;
using InvVocab = std::unordered_map<size_t, std::wstring>;

class BasicTokenizer {
public:
    BasicTokenizer(bool doLowerCase);
    std::vector<std::wstring> tokenize(const std::string& text) const;

private:
    std::wstring cleanText(const std::wstring& text) const;
    bool isControol(const wchar_t& ch) const;
    bool isWhitespace(const wchar_t& ch) const;
    bool isPunctuation(const wchar_t& ch) const;
    bool isChineseChar(const wchar_t& ch) const;
    std::wstring tokenizeChineseChars(const std::wstring& text) const;
    bool isStripChar(const wchar_t& ch) const;
    std::wstring strip(const std::wstring& text) const;
    std::vector<std::wstring> split(const std::wstring& text) const;
    std::wstring runStripAccents(const std::wstring& text) const;
    std::vector<std::wstring> runSplitOnPunc(const std::wstring& text) const;

    bool mDoLowerCase;
};

class WordpieceTokenizer {
public:
    WordpieceTokenizer(std::shared_ptr<Vocab> vocab, const std::wstring& unkToken = L"[UNK]", size_t maxInputCharsPerWord=200);
    std::vector<std::wstring> tokenize(const std::wstring& text) const;

private:
    std::shared_ptr<Vocab> mVocab;
    std::wstring mUnkToken;
    size_t mMaxInputCharsPerWord;
};

class FullTokenizer {
public:
    FullTokenizer(const std::string& vocabFile, bool doLowerCase = true);
    std::vector<std::wstring> tokenize(const std::string& text) const;
    std::vector<size_t> convertTokensToIds(const std::vector<std::wstring>& text) const;

private:
    std::shared_ptr<Vocab> mVocab;
    InvVocab mInvVocab;
    std::string mVocabFile;
    bool mDoLowerCase;
    BasicTokenizer mBasicTokenizer;
    WordpieceTokenizer mWordpieceTokenizer;
};

static std::string normalize_nfd(const std::string& s) {
    std::string ret;
    char *result = (char *) utf8proc_NFD((unsigned char *)s.c_str());
    if (result) {
        ret = std::string(result);
        free(result);
        result = NULL;
    } 
    return ret;
}

static bool isStripChar(const wchar_t& ch) {
    return stripChar.find(ch) != std::wstring::npos;
}

static std::wstring strip(const std::wstring& text) {
    std::wstring ret =  text;
    if (ret.empty()) return ret;
    size_t pos = 0;
    while (pos < ret.size() && isStripChar(ret[pos])) pos++;
    if (pos != 0) ret = ret.substr(pos, ret.size() - pos);
    pos = ret.size() - 1;
    while (pos != (size_t)-1 && isStripChar(ret[pos])) pos--;
    return ret.substr(0, pos + 1);
}


std::vector<std::wstring> split(const std::wstring& text, const std::wstring& delimiters) {
    std::vector<std::wstring> result;
    std::wistringstream iss(text);
    std::wstring token;

    while (std::getline(iss, token)) {
        size_t start = 0, end;
        while ((end = token.find_first_of(delimiters, start)) != std::wstring::npos) {
            if (start != end) {  // Ignore empty tokens
                result.push_back(token.substr(start, end - start));
            }
            start = end + 1;
        }
        if (start != token.length()) {  // Add the last token
            result.push_back(token.substr(start));
        }
    }

    return result;
}


std::wstring join(const std::vector<std::wstring>& vec, const std::wstring& delimiter) {
    std::wstringstream ss;
    for(size_t i = 0; i < vec.size(); ++i) {
        if(i != 0)
            ss << delimiter;
        ss << vec[i];
    }
    return ss.str();
}


static std::vector<std::wstring> split(const std::wstring& text) {
    std::vector<std::wstring> result = split(text, stripChar);
    return result;
}

static std::vector<std::wstring> whitespaceTokenize(const std::wstring& text) {
    std::wstring rtext = strip(text);
    if (rtext.empty()) return std::vector<std::wstring>();
    return split(text);
}

static std::wstring convertToUnicode(const std::string& text) {
    size_t i = 0;
    std::wstring ret;
    while (i < text.size()) {
        wchar_t codepoint;
        utf8proc_ssize_t forward = utf8proc_iterate((utf8proc_uint8_t *)&text[i], text.size() - i, (utf8proc_int32_t*)&codepoint);
        if (forward < 0) return L"";
        ret += codepoint;
        i += forward;
    }
    return ret;
}

static std::string convertFromUnicode(const std::wstring& wText) {
    char dst[64];
    std::string ret;
    for (auto ch : wText) {
        utf8proc_ssize_t num = utf8proc_encode_char(ch, (utf8proc_uint8_t *)dst);
        if (num <= 0) return "";
        ret += std::string(dst, dst+num);
    }
    return ret;
}

static std::wstring tolower(const std::wstring& s) {
    std::wstring ret(s.size(), L' ');
    for (size_t i = 0; i < s.size(); i++) {
        ret[i] = utf8proc_tolower(s[i]);
    }
    return ret;
}

static std::shared_ptr<Vocab> loadVocab(const std::string& vocabFile) {
    std::shared_ptr<Vocab> vocab(new Vocab);
    size_t index = 0;
    std::ifstream ifs(vocabFile, std::ifstream::in);
    std::string line;
    while (getline(ifs, line)) {
        std::wstring token = convertToUnicode(line);
        if (token.empty()) break;
        token = strip(token);
        (*vocab)[token] = index;
        index++;
    }
    return vocab;
}

BasicTokenizer::BasicTokenizer(bool doLowerCase=true) 
    : mDoLowerCase(doLowerCase) {
}

std::wstring BasicTokenizer::cleanText(const std::wstring& text) const {
    std::wstring output;
    for (const wchar_t& cp : text)  {
        if (cp == 0 || cp == 0xfffd || isControol(cp)) continue;
        if (isWhitespace(cp)) output += L" ";
        else output += cp;
    }
    return output;
}

bool BasicTokenizer::isControol(const wchar_t& ch) const {
    if (ch== L'\t' || ch== L'\n' || ch== L'\r') return false;
    auto cat = utf8proc_category(ch);
    if (cat == UTF8PROC_CATEGORY_CC || cat == UTF8PROC_CATEGORY_CF) return true;
    return false;
}

bool BasicTokenizer::isWhitespace(const wchar_t& ch) const {
    if (ch== L' ' || ch== L'\t' || ch== L'\n' || ch== L'\r') return true;
    auto cat = utf8proc_category(ch);
    if (cat == UTF8PROC_CATEGORY_ZS) return true;
    return false;
}

bool BasicTokenizer::isPunctuation(const wchar_t& ch) const {
    if ((ch >= 33 && ch <= 47) || (ch >= 58 && ch <= 64) || 
        (ch >= 91 && ch <= 96) || (ch >= 123 && ch <= 126)) return true;
    auto cat = utf8proc_category(ch);
    if (cat == UTF8PROC_CATEGORY_PD || cat == UTF8PROC_CATEGORY_PS
            || cat == UTF8PROC_CATEGORY_PE || cat == UTF8PROC_CATEGORY_PC
            || cat == UTF8PROC_CATEGORY_PO //sometimes ¶ belong SO
            || cat == UTF8PROC_CATEGORY_PI
            || cat == UTF8PROC_CATEGORY_PF) return true;
    return false;
}

bool BasicTokenizer::isChineseChar(const wchar_t& ch) const {
    if ((ch >= 0x4E00 && ch <= 0x9FFF) ||
        (ch >= 0x3400 && ch <= 0x4DBF) ||
        (ch >= 0x20000 && ch <= 0x2A6DF) ||  
        (ch >= 0x2A700 && ch <= 0x2B73F) ||
        (ch >= 0x2B740 && ch <= 0x2B81F) ||
        (ch >= 0x2B820 && ch <= 0x2CEAF) ||
        (ch >= 0xF900 && ch <= 0xFAFF) ||
        (ch >= 0x2F800 && ch <= 0x2FA1F))
        return true;
    return false;
}

std::wstring BasicTokenizer::tokenizeChineseChars(const std::wstring& text) const {
    std::wstring output;
    for (auto& ch : text) {
        if (isChineseChar(ch)) {
            output += L' ';
            output += ch;
            output += L' ';
        }
        else
            output += ch;
    }
    return output;
}

std::wstring BasicTokenizer::runStripAccents(const std::wstring& text) const {
    //Strips accents from a piece of text.
    std::wstring nText;
    try {
        nText = convertToUnicode(normalize_nfd(convertFromUnicode(text)));
    } catch (std::bad_cast& e) {
        std::cerr << "bad_cast" << std::endl;
        return L"";  
    }
    
    std::wstring output;
    for (auto& ch : nText) {
        auto cat = utf8proc_category(ch);
        if (cat == UTF8PROC_CATEGORY_MN) continue;
        output += ch;
    }
    return output;
}

std::vector<std::wstring> BasicTokenizer::runSplitOnPunc(const std::wstring& text) const {
    size_t i = 0;
    bool startNewWord = true;
    std::vector<std::wstring> output;
    while (i < text.size()) {
        wchar_t ch = text[i];
        if (isPunctuation(ch)) {
            output.push_back(std::wstring(&ch, 1));
            startNewWord = true;
        }
        else {
            if (startNewWord) output.push_back(std::wstring());
            startNewWord = false;
            output[output.size() - 1] += ch;
        }
        i++;
    }
    return output;
}

std::vector<std::wstring> BasicTokenizer::tokenize(const std::string& text) const {
    std::wstring nText = convertToUnicode(text);
    nText = cleanText(nText);

    nText = tokenizeChineseChars(nText);

    const std::vector<std::wstring>& origTokens = whitespaceTokenize(nText);
    std::vector<std::wstring> splitTokens;
    for (std::wstring token : origTokens) {
        if (mDoLowerCase) {
            token = tolower(token);
            token = runStripAccents(token);
        }
        const auto& tokens = runSplitOnPunc(token);
        splitTokens.insert(splitTokens.end(), tokens.begin(), tokens.end());
    }
    return whitespaceTokenize(join(splitTokens, L" "));
}

WordpieceTokenizer::WordpieceTokenizer(const std::shared_ptr<Vocab> vocab, const std::wstring& unkToken, size_t maxInputCharsPerWord)
    : mVocab(vocab),
    mUnkToken(unkToken),
    mMaxInputCharsPerWord(maxInputCharsPerWord) {
}

std::vector<std::wstring> WordpieceTokenizer::tokenize(const std::wstring& text) const {
    std::vector<std::wstring> outputTokens;
    for (auto& token : whitespaceTokenize(text)) {
        if (token.size() > mMaxInputCharsPerWord) {
            outputTokens.push_back(mUnkToken);
        }
        bool isBad = false;
        size_t start = 0;
        std::vector<std::wstring> subTokens;
        while (start < token.size()) {
            size_t end = token.size();
            std::wstring curSubstr;
            bool hasCurSubstr = false;
            while (start < end) {
                std::wstring substr = token.substr(start, end - start);
                if (start > 0) substr = L"##" + substr;
                if (mVocab->find(substr) != mVocab->end()) {
                    curSubstr = substr;
                    hasCurSubstr = true;
                    break;
                }
                end--;
            }
            if (!hasCurSubstr) {
                isBad = true;
                break;
            }
            subTokens.push_back(curSubstr);
            start = end;
        }
        if (isBad) outputTokens.push_back(mUnkToken);
        else outputTokens.insert(outputTokens.end(), subTokens.begin(), subTokens.end());
    }
    return outputTokens;
}

FullTokenizer::FullTokenizer(const std::string& vocabFile, bool doLowerCase) : 
    mVocab(loadVocab(vocabFile)), 
    mBasicTokenizer(BasicTokenizer(doLowerCase)),
    mWordpieceTokenizer(WordpieceTokenizer(mVocab)) {
    for (auto& v : *mVocab) mInvVocab[v.second] = v.first;
}

std::vector<std::wstring> FullTokenizer::tokenize(const std::string& text) const {
    std::vector<std::wstring> splitTokens;
    for (auto& token : mBasicTokenizer.tokenize(text))
        for (auto& subToken : mWordpieceTokenizer.tokenize(token))  
            splitTokens.push_back(subToken);
    return splitTokens;
}

std::vector<size_t> FullTokenizer::convertTokensToIds(const std::vector<std::wstring>& text) const {
    std::vector<size_t> ret(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        ret[i] = (*mVocab)[text[i]];
    }
    return ret;
}




class Model {
public:
    Model(const std::string& model_path, const std::string& vocab_path);
    ~Model() {delete tokenizer_; delete ses_;}
    // 执行文本预测，返回预测的分类名称
    std::string predict(const std::string& text, float* score=nullptr);

    // 执行文本预测，返回预测的分类ID
    int infer(const std::string& text, float* score=nullptr);

protected:
    // 将文本向量化，返回ids和mask两个向量
    std::vector<std::vector<int64_t>> build_input(const std::string& text);

private:
    FullTokenizer* tokenizer_ = nullptr;
    Ort::Session* ses_ = nullptr;
    Ort::Env env_; // 注意
};


const static std::vector<std::string> kNames = { 
    "finance",
    "realty",
    "stocks",
    "education",
    "science",
    "society",
    "politics",
    "sports",
    "game",
    "entertainment"
};

int Model::infer(const std::string& text, float* score) {
    auto& session = *ses_;
    // 调用前面的build_input
    auto res = build_input(text);
    std::vector<int64_t> shape = {1, 32};

    auto& input_tensor_values = res[0];
    auto& mask_tensor_values = res[1];

    const static auto memory_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    Ort::Value input_tensor = Ort::Value::CreateTensor<int64_t>(memory_info, input_tensor_values.data(),
                                                            input_tensor_values.size(), shape.data(), 2);

    Ort::Value mask_tensor = Ort::Value::CreateTensor<int64_t>(memory_info, mask_tensor_values.data(),
                                                            mask_tensor_values.size(), shape.data(), 2);

    std::vector<Ort::Value> ort_inputs;
    ort_inputs.push_back(std::move(input_tensor));
    ort_inputs.push_back(std::move(mask_tensor));

    const static std::vector<const char*> input_node_names = {"ids", "mask"};
    const static std::vector<const char*> output_node_names = {"output"};
    auto output_tensors = session.Run(Ort::RunOptions{nullptr}, input_node_names.data(), ort_inputs.data(),
                                    ort_inputs.size(), output_node_names.data(), 1);

    if (output_tensors.size() != output_node_names.size()) {
        return -1;
    }
    const float* output = output_tensors[0].GetTensorData<float>();

    int idx = argmax(output, output+10);
    if (score != nullptr) {
        *score = output[idx];
    }
    return idx;
}

std::string Model::predict(const std::string& text, float* score) {
    int idx = infer(text, score);
    return (idx >= 0 && idx < kNames.size()) ? kNames[idx] : "Unknown";
}}

Model::Model(const std::string& model_path, 
             const std::string& vocab_path)
                 :env_(ORT_LOGGING_LEVEL_WARNING, "test") {
    tokenizer_ = new FullTokenizer(vocab_path);

    Ort::SessionOptions session_options;
    OrtCUDAProviderOptions cuda_options;
    session_options.AppendExecutionProvider_CUDA(cuda_options);
    ses_ = new Ort::Session(env_, model_path.c_str(), session_options);
}



std::vector<std::vector<int64_t>> Model::build_input(const std::string& text) { 
    auto tokens = tokenizer_->tokenize(text);
    auto token_ids = tokenizer_->convertTokensToIds(tokens);

    std::vector<std::vector<int64_t>> res;

    std::vector<int64_t> input(32);
    std::vector<int64_t> mask(32);
    input[0] = 101; // Bert模型的[CLS]标记的
    mask[0] = 1;
    for (int i = 0; i < token_ids.size() && i < 31; ++i) {
        input[i+1] = token_ids[i];
        mask[i+1] = token_ids[i] > 0;
    }
    res.push_back(std::move(input));
    res.push_back(std::move(mask));
    return res;
}
