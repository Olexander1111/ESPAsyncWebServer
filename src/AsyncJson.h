#ifndef ASYNC_JSON_H_
#define ASYNC_JSON_H_

#include <Arduino.h>
#include <Print.h>
#include <StringUtils.h>
#include <ESPAsyncWebServer.h>
#include <GSON.h>
#include <Ticker.h>
#include <stdlib.h>

constexpr const char* JSON_MIMETYPE = "application/json";

#ifndef CHUNK_OBJ_SIZE
#define CHUNK_OBJ_SIZE 1024 
#endif

#ifndef CHUNK_PROCESS_PERIOD_MS
#define CHUNK_PROCESS_PERIOD_MS 3 
#endif

#ifndef MAX_JSON_CONTENT_LENGTH
#define MAX_JSON_CONTENT_LENGTH 16384
#endif

class ChunkPrint : public Print {
private:
    uint8_t* _destination;
    size_t _offset;
    size_t _remaining;
    size_t _position;

public:
    ChunkPrint(uint8_t* destination, size_t offset, size_t length) noexcept
        : _destination(destination), _offset(offset), _remaining(length), _position(0) {}
        
    size_t write(uint8_t byte) override {
        if (_offset > 0) {
            --_offset;
            return 1;
        }
        
        if (_remaining == 0) return 0;
        
        _destination[_position++] = byte;
        --_remaining;
        return 1;
    }
    
    size_t write(const uint8_t* buffer, size_t size) override {
        if (!buffer || size == 0) return 0;
        
        // Skip bytes if we haven't reached our start position
        if (_offset >= size) {
            _offset -= size;
            return size;
        }
        
        // Handle partial skip
        if (_offset > 0) {
            buffer += _offset;
            size -= _offset;
            _offset = 0;
        }
        
        // Copy what we can
        const size_t copySize = std::min(size, _remaining);
        if (copySize > 0) {
            memcpy(_destination + _position, buffer, copySize);
            _position += copySize;
            _remaining -= copySize;
        }
        
        return size; // Return total processed (not just copied)
    }
    
    size_t getWrittenBytes() const noexcept { return _position; }
    size_t getRemainingBytes() const noexcept { return _remaining; }
};

class AsyncJsonResponse : public AsyncAbstractResponse {
private:
    gson::string _jsonBuffer;
    bool _isValid;

public:
    AsyncJsonResponse() noexcept : _isValid(false) {
        _code = 200;
        _contentType = JSON_MIMETYPE;
        _contentLength = 0;
    }

    ~AsyncJsonResponse() = default;
    
    // Move semantics for better performance
    AsyncJsonResponse(AsyncJsonResponse&& other) noexcept 
        : AsyncAbstractResponse(std::move(other))
        , _jsonBuffer(std::move(other._jsonBuffer))
        , _isValid(other._isValid) {
        other._isValid = false;
    }
    
    AsyncJsonResponse& operator=(AsyncJsonResponse&& other) noexcept {
        if (this != &other) {
            AsyncAbstractResponse::operator=(std::move(other));
            _jsonBuffer = std::move(other._jsonBuffer);
            _isValid = other._isValid;
            other._isValid = false;
        }
        return *this;
    }

    gson::string& getRoot() noexcept { return _jsonBuffer; }
    const gson::string& getRoot() const noexcept { return _jsonBuffer; }
    
    bool _sourceValid() const noexcept override { return _isValid; }
    
    size_t setLength() {
        _contentLength = _jsonBuffer.s.length();
        _isValid = _contentLength > 0;
        return _contentLength;
    }

    size_t getSize() const noexcept { return _jsonBuffer.s.length(); }

protected:
    size_t _fillBuffer(uint8_t* data, size_t len) override {
        if (!data || len == 0 || !_isValid) return 0;
        
        const size_t jsonSize = _jsonBuffer.s.length();
        if (_sentLength >= jsonSize) return 0;
        
        ChunkPrint dest(data, 0, len);
        const size_t remainingData = jsonSize - _sentLength;
        const size_t bytesToWrite = std::min(len, remainingData);
        
        dest.write(reinterpret_cast<const uint8_t*>(_jsonBuffer.s.c_str() + _sentLength), bytesToWrite);
        return dest.getWrittenBytes();
    }
};

// Custom deleter for malloc'd memory
struct MallocDeleter {
    void operator()(uint8_t* ptr) const {
        if (ptr) free(ptr);
    }
};

// Base class for common JSON handler functionality
class AsyncJsonHandlerBase : public AsyncWebHandler {
protected:
    const String _uri;
    WebRequestMethodComposite _method;
    size_t _maxContentLength;
    size_t _contentLength;
    std::unique_ptr<uint8_t[], MallocDeleter> _buffer;
    size_t _bufferSize;
    bool _bufferReady;

    bool validateRequest(AsyncWebServerRequest* request) const {
        if (!(_method & request->method())) return false;
        if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/"))) return false;
        if (!request->contentType().equalsIgnoreCase(JSON_MIMETYPE)) return false;
        return true;
    }

    bool allocateBuffer(size_t size) {
        if (size > _maxContentLength) return false;
        if (!_buffer || _bufferSize < size) {
            auto tmp = static_cast<uint8_t*>(malloc(size));
            if (!tmp) return false;
            _buffer.reset(tmp);
            _bufferSize = size;
        }
        _bufferReady = true;
        return true;
    }

public:
    AsyncJsonHandlerBase(const String& uri, WebRequestMethodComposite method = HTTP_POST | HTTP_PUT | HTTP_PATCH) 
        : _uri(uri), _method(method), _maxContentLength(MAX_JSON_CONTENT_LENGTH)
        , _contentLength(0), _bufferSize(0), _bufferReady(false) {}

    virtual ~AsyncJsonHandlerBase() = default;

    void setMethod(WebRequestMethodComposite method) noexcept { _method = method; }
    void setMaxContentLength(size_t maxContentLength) noexcept { _maxContentLength = maxContentLength; }

    void handleUpload(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) override final {}

    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) override final {
        _contentLength = total;
        if (total == 0 || !data || len == 0) return;

        // Allocate buffer on first call
        if (!_buffer && !allocateBuffer(total)) {
            return; // Buffer allocation failed or size exceeded
        }

        // Bounds checking
        if (index + len <= _bufferSize) {
            memcpy(_buffer.get() + index, data, len);
        }
    }
};

// Optimized callback-based JSON handler
class AsyncCallbackJsonWebHandler : public AsyncJsonHandlerBase {
public:
    using JsonRequestHandlerFunction = std::function<void(AsyncWebServerRequest*, gson::Entry&)>;

private:
    JsonRequestHandlerFunction _onRequest;

public:
    AsyncCallbackJsonWebHandler(const String& uri, JsonRequestHandlerFunction onRequest) 
        : AsyncJsonHandlerBase(uri), _onRequest(std::move(onRequest)) {}

    void onRequest(JsonRequestHandlerFunction fn) { _onRequest = std::move(fn); }

    bool canHandle(AsyncWebServerRequest* request) override final {
        if (!_onRequest) return false;
        if (!validateRequest(request)) return false;
        request->addInterestingHeader(F("ANY"));
        return true;
    }

    void handleRequest(AsyncWebServerRequest* request) override final {
        if (!_onRequest) {
            request->send(500, F("text/plain"), F("No handler configured"));
            return;
        }

        if (_contentLength > _maxContentLength) {
            request->send(413, F("text/plain"), F("Content too large"));
            return;
        }

        if (!_bufferReady || !_buffer) {
            request->send(400, F("text/plain"), F("Invalid request body"));
            return;
        }

        // Parse JSON with proper error
        gson::Parser parser;
        bool parseSuccess = parser.parse(reinterpret_cast<char*>(_buffer.get()), _contentLength);

        if (!parseSuccess || parser.hasError()) {
            request->send(400, F("text/plain"), F("Invalid JSON"));
            _bufferReady = false;
            return;
        }

        // Create Entry from the root of the parser for the callback
        gson::Entry jsonRoot = parser.get(0); // Get root element
        
        // Call the user handler - they're responsible for sending response
        _onRequest(request, jsonRoot);

        // Reset buffer for next request
        _bufferReady = false;
    }

    bool isRequestHandlerTrivial() override final { return !_onRequest; }
};

class AsyncJsonStreamCallback : public AsyncJsonHandlerBase {
public:
    using JsonStreamHandlerFunction = std::function<void(AsyncWebServerRequest*, gson::string&)>;

private:
    JsonStreamHandlerFunction _onJsonStreamRequest;
    AsyncWebServerRequest* _currentRequest;
    size_t _processIndex;
    Ticker _chunkTimer;

    void processNextChunk() {
        if (!_currentRequest || !_buffer || _processIndex >= _contentLength) {
            cleanup();
            return;
        }

#ifdef ESP8266
        // Process in chunks for ESP8266
        const size_t chunkSize = std::min(static_cast<size_t>(CHUNK_OBJ_SIZE), _contentLength - _processIndex);
        
        if (chunkSize > 0) {
            gson::string rawJson;
            rawJson.addTextRaw(reinterpret_cast<char*>(_buffer.get() + _processIndex), chunkSize);
            
            _onJsonStreamRequest(_currentRequest, rawJson);
            _processIndex += chunkSize;
            
            if (_processIndex < _contentLength) {
                _chunkTimer.once_ms(CHUNK_PROCESS_PERIOD_MS, [this]() { processNextChunk(); });
            } else {
                cleanup();
            }
        } else {
            cleanup();
        }
#else
        // Process all at once for ESP32
        gson::string rawJson;
        rawJson.addTextRaw(reinterpret_cast<char*>(_buffer.get()), _contentLength);
        _onJsonStreamRequest(_currentRequest, rawJson);
        cleanup();
#endif
    }

    void cleanup() {
        _currentRequest = nullptr;
        _processIndex = 0;
        _bufferReady = false;
        _chunkTimer.detach();
    }

public:
    AsyncJsonStreamCallback(const String& uri, JsonStreamHandlerFunction onRequest)
        : AsyncJsonHandlerBase(uri), _onJsonStreamRequest(std::move(onRequest))
        , _currentRequest(nullptr), _processIndex(0) {}

    ~AsyncJsonStreamCallback() {
        cleanup();
    }

    void onRequest(JsonStreamHandlerFunction fn) { _onJsonStreamRequest = std::move(fn); }

    bool canHandle(AsyncWebServerRequest* request) override final {
        if (!_onJsonStreamRequest) return false;
        if (!validateRequest(request)) return false;
        request->addInterestingHeader(F("ANY"));
        return true;
    }

    void handleRequest(AsyncWebServerRequest* request) override final {
        if (!_onJsonStreamRequest) {
            request->send(500, F("text/plain"), F("No handler configured"));
            return;
        }

        if (_contentLength > _maxContentLength) {
            request->send(413, F("text/plain"), F("Content too large"));
            return;
        }

        if (!_bufferReady || !_buffer || _contentLength == 0) {
            request->send(400, F("text/plain"), F("Invalid request body"));
            return;
        }

        _currentRequest = request;
        _processIndex = 0;
        processNextChunk();
    }

    bool isRequestHandlerTrivial() override final { return !_onJsonStreamRequest; }
};

// Type aliases for backward compatibility
using JsonRequestHandlerFunction    = AsyncCallbackJsonWebHandler::JsonRequestHandlerFunction;
using JsonStreamHandlerFunction     = AsyncJsonStreamCallback::JsonStreamHandlerFunction;

#endif // ASYNC_JSON_H_
