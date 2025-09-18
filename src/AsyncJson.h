#ifndef ASYNC_JSON_H_
#define ASYNC_JSON_H_
#include <Arduino.h>
#include <Print.h>
#include <StringUtils.h>
#include <ESPAsyncWebServer.h>
#include <GSON.h>
#include <Ticker.h>

constexpr const char* JSON_MIMETYPE = "application/json";

#ifndef CHUNK_OBJ_SIZE
#define CHUNK_OBJ_SIZE 768
#endif

#ifndef CHUNK_PROCESS_PERIOD_MS
#define CHUNK_PROCESS_PERIOD_MS 5
#endif

class ChunkPrint : public Print {

private:
    uint8_t* _destination;
    size_t _to_skip;
    size_t _to_write;
    size_t _pos;

public:
    ChunkPrint(uint8_t* destination, size_t from, size_t len)
        : _destination(destination), _to_skip(from), _to_write(len), _pos(0) {}
        
    size_t write(uint8_t c) override {
        if (_to_skip > 0) {
            _to_skip--;
            return 1;
        }
        
        if (_to_write == 0) return 0;
        
        _destination[_pos++] = c;
        _to_write--;
        return 1;
    }
    
    size_t write(const uint8_t *buffer, size_t size) override {
        if (_to_skip >= size) {
            _to_skip -= size;
            return size;
        }
        
        if (_to_skip > 0) {
            buffer += _to_skip;
            size -= _to_skip;
            _to_skip = 0;
        }
        
        size_t to_copy = (size < _to_write) ? size : _to_write;
        if (to_copy > 0) {
            memcpy(_destination + _pos, buffer, to_copy);
            _pos += to_copy;
            _to_write -= to_copy;
        }
        
        return to_copy + (size - to_copy); // Return actual written + skipped
    }
};

class AsyncJsonResponse : public AsyncAbstractResponse {
protected:
    gson::string _jsonBuffer;
    bool _isValid;

public:
    AsyncJsonResponse() : _isValid{ false } {
        _code = 200;
        _contentType = JSON_MIMETYPE;
    }

    ~AsyncJsonResponse() {}
    gson::string &getRoot() { return _jsonBuffer; }
    bool _sourceValid() const { return _isValid; }
    size_t setLength() {
        _contentLength = _jsonBuffer.s.length();
        if (_contentLength) { _isValid = true; }
        return _contentLength;
    }

    size_t getSize() { return _jsonBuffer.s.length(); }

    size_t _fillBuffer(uint8_t *data, size_t len) {
        ChunkPrint dest(data, _sentLength, len);
        dest.write(reinterpret_cast<const uint8_t*>(_jsonBuffer.s.c_str()), _jsonBuffer.s.length());
        return len;
    }
};

typedef std::function<void(AsyncWebServerRequest *request, gson::Entry &json)> JsonRequestHandlerFunction;
typedef std::function<void(AsyncWebServerRequest *request, gson::string &json)> JsonStreamHandlerFunction;

class AsyncCallbackJsonWebHandler : public AsyncWebHandler {
private:
    const String _uri;
    WebRequestMethodComposite _method;
    JsonRequestHandlerFunction _onRequest;

    size_t _contentLength;
    size_t _maxContentLength;
    std::unique_ptr<uint8_t[]> _tempBuffer;
    size_t _tempObjectSize;

public:
    AsyncCallbackJsonWebHandler(const String& uri, JsonRequestHandlerFunction onRequest) 
        : _uri(uri), _method(HTTP_POST | HTTP_PUT | HTTP_PATCH), _onRequest(onRequest), _maxContentLength(8096), _tempBuffer(nullptr), _tempObjectSize(0) {}

    void setMethod(WebRequestMethodComposite method) { _method = method; }
    void setMaxContentLength(int maxContentLength) { _maxContentLength = maxContentLength; }
    void onRequest(JsonRequestHandlerFunction fn) { _onRequest = fn; }

    virtual bool canHandle(AsyncWebServerRequest *request) override final {
        if (!_onRequest)
            return false;

        if (!(_method & request->method()))
            return false;

        if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/")))
            return false;

        if (!request->contentType().equalsIgnoreCase(JSON_MIMETYPE))
            return false;

        request->addInterestingHeader("ANY");
        return true;
    }

    virtual void handleUpload(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) override final {}

   virtual void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) override final {
    if (!_onRequest) return;

    _contentLength = total;
    if (total == 0) return;

    // Allocate buffer only once at the beginning using unique_ptr
    if (!_tempBuffer) {
        if (total > _maxContentLength) {
            // Too large, skip allocation; handleRequest() will reply with 413
            return;
        }
        _tempBuffer.reset(new (std::nothrow) uint8_t[total]);
        if (!_tempBuffer) {
            _tempObjectSize = 0;
            return;
        }
        _tempObjectSize = total;
    }

    // Copy safely if within bounds
    if (_tempBuffer && index + len <= _tempObjectSize) {
        memcpy(_tempBuffer.get() + index, data, len);
    } else {
        // Out-of-bounds or allocation failure — ignore or log error
    }
}
    virtual bool isRequestHandlerTrivial() override final { return _onRequest ? false : true; }
};

class AsyncJsonStreamCallback : public AsyncWebHandler {
private:
    const String _uri;
    WebRequestMethodComposite _method;
    JsonStreamHandlerFunction _onJsonStreamRequest;

    size_t _contentLength;
    size_t _maxContentLength;
    std::unique_ptr<uint8_t[]> _tempBuffer;
    size_t _tempObjectSize;

    AsyncWebServerRequest* _request;
    size_t _index;
    Ticker _nextChunkTimer;

    void processNextChunk() {
#ifdef ESP8266        
        const size_t CHUNK_SIZE = CHUNK_OBJ_SIZE;
        if (_index < _tempObjectSize) {
            size_t chunkLen = (_index + CHUNK_SIZE < _tempObjectSize) ? CHUNK_SIZE : (_tempObjectSize - _index);
            std::unique_ptr<char[]> chunkObject(new char[chunkLen]);
            memcpy(chunkObject.get(), _tempBuffer.get() + _index, chunkLen);

            gson::string rawJson;
            rawJson.addTextRaw(chunkObject.get(), chunkLen);
            _onJsonStreamRequest(_request, rawJson);

            _index += chunkLen;
            _nextChunkTimer.once_ms(CHUNK_PROCESS_PERIOD_MS, [this]() { this->processNextChunk(); });
        } else {
            _tempBuffer.reset();
            _tempObjectSize = 0;
        }
#endif

#ifdef ESP32
        if (_tempObjectSize > 0) {
            std::unique_ptr<char[]> fullObject(new char[_tempObjectSize]);
            memcpy(fullObject.get(), _tempBuffer.get(), _tempObjectSize);

            gson::string rawJson;
            rawJson.addTextRaw(fullObject.get(), _tempObjectSize);
            _onJsonStreamRequest(_request, rawJson);

            _tempBuffer.reset();
            _tempObjectSize = 0;
        }
#endif
    }

public:
    AsyncJsonStreamCallback(const String& uri, JsonStreamHandlerFunction onRequest)
        : _uri(uri), _method(HTTP_POST | HTTP_PUT | HTTP_PATCH), _onJsonStreamRequest(onRequest), _maxContentLength(16384), _tempBuffer(nullptr), _tempObjectSize(0) {}

    void setMethod(WebRequestMethodComposite method) { _method = method; }
    void setMaxContentLength(int maxContentLength) { _maxContentLength = maxContentLength; }
    void onRequest2(JsonStreamHandlerFunction fn) { _onJsonStreamRequest = fn; }

    virtual bool canHandle(AsyncWebServerRequest *request) override final {
        if (!_onJsonStreamRequest) return false;
        if (!(_method & request->method())) return false;
        if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/"))) return false;
        if (!request->contentType().equalsIgnoreCase(JSON_MIMETYPE)) return false;
        request->addInterestingHeader("ANY");
        return true;
    }

    virtual void handleRequest(AsyncWebServerRequest *request) override final {
        if (_onJsonStreamRequest) {
            if (_tempBuffer && _tempObjectSize > 0) {
                _request = request;
                _index = 0;
                processNextChunk();
            } else {
                request->send(_contentLength > _maxContentLength ? 413 : 400);
            }
        } else {
            request->send(500);
        }
    }

    virtual void handleUpload(AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) override final {}

    virtual void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) override final {
        _contentLength = total;
        if (total == 0) return;

        if (!_tempBuffer) {
            if (total > _maxContentLength) return;
            _tempBuffer.reset(new (std::nothrow) uint8_t[total]);
            if (!_tempBuffer) { _tempObjectSize = 0; return; }
            _tempObjectSize = total;
        }

        if (_tempBuffer && index + len <= _tempObjectSize) {
            memcpy(_tempBuffer.get() + index, data, len);
        }
    }

    virtual bool isRequestHandlerTrivial() override final { return _onJsonStreamRequest ? false : true; }
};
#endif
