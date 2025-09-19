#ifndef ASYNC_JSON_H_
#define ASYNC_JSON_H_

#include <Arduino.h>
#include <Print.h>
#include <StringUtils.h>
#include <ESPAsyncWebServer.h>
#include <GSON.h>
#include <memory>
#include <functional>

#ifdef ESP8266
#include <Ticker.h>
#elif defined(ESP32)
#include <esp_timer.h>
#endif

constexpr const char* JSON_MIMETYPE = "application/json";

#ifndef CHUNK_OBJ_SIZE
#define CHUNK_OBJ_SIZE 512
#endif

#ifndef CHUNK_PROCESS_PERIOD_MS
#define CHUNK_PROCESS_PERIOD_MS 3
#endif

#ifndef MAX_JSON_CONTENT_LENGTH
#define MAX_JSON_CONTENT_LENGTH 16384
#endif

// ---------------------------
// Memory management utilities
// ---------------------------
struct MallocDeleter {
    void operator()(uint8_t* ptr) const noexcept { 
        if (ptr) free(ptr); 
    }
};

// ---------------------------
// Improved ChunkPrint with bounds checking
// ---------------------------
class ChunkPrint : public Print {
private:
    uint8_t* _destination;
    size_t _to_skip;
    size_t _to_write;
    size_t _pos;
    size_t _max_size;  // Added for bounds checking

public:
    ChunkPrint(uint8_t* destination, size_t from, size_t len, size_t max_size) noexcept
        : _destination(destination), _to_skip(from), _to_write(len), _pos(0), _max_size(max_size) {}

    size_t write(uint8_t c) override {
        if (_to_skip > 0) {
            _to_skip--;
            return 1;
        }
        if (_to_write > 0 && _pos < _max_size) {
            _destination[_pos++] = c;
            _to_write--;
            return 1;
        }
        return 0;
    }

    size_t write(const uint8_t* buffer, size_t size) override {
        if (!buffer || size == 0) return 0;
        
        // Skip bytes if needed
        if (_to_skip > 0) {
            const size_t skip_amount = std::min(_to_skip, size);
            _to_skip -= skip_amount;
            buffer += skip_amount;
            size -= skip_amount;
            if (size == 0) return skip_amount;
        }

        // Write remaining bytes with bounds checking
        const size_t available_space = std::min({_to_write, _max_size - _pos, size});
        if (available_space > 0) {
            memcpy(_destination + _pos, buffer, available_space);
            _pos += available_space;
            _to_write -= available_space;
        }
        
        return size;  // Return original size to maintain compatibility
    }

    size_t getWrittenBytes() const noexcept { return _pos; }
    size_t getRemainingSpace() const noexcept { return _to_write; }
};

// ---------------------------
// Improved AsyncJsonResponse with better error handling
// ---------------------------
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

    // Move constructor and assignment
    AsyncJsonResponse(AsyncJsonResponse&& other) noexcept 
        : AsyncAbstractResponse(std::move(other)), 
          _jsonBuffer(std::move(other._jsonBuffer)), 
          _isValid(other._isValid) {
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
        
        const size_t json_size = _jsonBuffer.s.length();
        if (_sentLength >= json_size) return 0;

        ChunkPrint dest(data, 0, len, len);
        const size_t remaining = json_size - _sentLength;
        const size_t to_write = std::min(len, remaining);
        
        dest.write(reinterpret_cast<const uint8_t*>(_jsonBuffer.s.c_str() + _sentLength), to_write);
        return dest.getWrittenBytes();
    }
};

// ---------------------------
// Base class with improved safety
// ---------------------------
class AsyncJsonHandlerBase : public AsyncWebHandler {
protected:
    const String _uri;
    WebRequestMethodComposite _method;
    size_t _maxContentLength;
    size_t _contentLength;
    std::unique_ptr<uint8_t[], MallocDeleter> _tempObject;
    size_t _tempObjectSize;
    bool _bufferReady;

    bool validateRequest(AsyncWebServerRequest* request) const noexcept {
        if (!request) return false;
        if (!(_method & request->method())) return false;
        if (_uri.length() && (_uri != request->url() && !request->url().startsWith(_uri + "/"))) return false;
        if (!request->contentType().equalsIgnoreCase(JSON_MIMETYPE)) return false;
        return true;
    }

    bool allocateBuffer(size_t size) noexcept {
        if (size == 0 || size > _maxContentLength) return false;
        
        if (!_tempObject || _tempObjectSize < size) {
            _tempObject.reset(static_cast<uint8_t*>(malloc(size)));
            if (!_tempObject) {
                _tempObjectSize = 0;
                _bufferReady = false;
                return false;
            }
            _tempObjectSize = size;
        }
        _bufferReady = true;
        return true;
    }

public:
    AsyncJsonHandlerBase(const String& uri, WebRequestMethodComposite method = HTTP_POST | HTTP_PUT | HTTP_PATCH) noexcept
        : _uri(uri), _method(method), _maxContentLength(MAX_JSON_CONTENT_LENGTH), 
          _contentLength(0), _tempObjectSize(0), _bufferReady(false) {}

    virtual ~AsyncJsonHandlerBase() = default;

    void setMethod(WebRequestMethodComposite method) noexcept { _method = method; }
    void setMaxContentLength(size_t maxContentLength) noexcept { _maxContentLength = maxContentLength; }

    void handleUpload(AsyncWebServerRequest*, const String&, size_t, uint8_t*, size_t, bool) override final {}

    void handleBody(AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) override final {
        if (!request || !data || len == 0) return;
        
        _contentLength = total;
        if (total > _maxContentLength) return;  // Will be handled in handleRequest

        if (!_tempObject && !allocateBuffer(total)) return;

        // Bounds checking for memcpy
        if (_tempObject && (index + len) <= _tempObjectSize) {
            memcpy(_tempObject.get() + index, data, len);
        }
    }
};

// ---------------------------
// Improved AsyncCallbackJsonWebHandler
// ---------------------------
class AsyncCallbackJsonWebHandler : public AsyncJsonHandlerBase {
public:
    using JsonRequestHandlerFunction = std::function<void(AsyncWebServerRequest*, gson::Entry&)>;

private:
    JsonRequestHandlerFunction _onRequest;

public:
    AsyncCallbackJsonWebHandler(const String& uri, JsonRequestHandlerFunction onRequest) noexcept
        : AsyncJsonHandlerBase(uri), _onRequest(std::move(onRequest)) {}

    void onRequest(JsonRequestHandlerFunction fn) { _onRequest = std::move(fn); }

    bool canHandle(AsyncWebServerRequest* request) override final {
        if (!_onRequest || !validateRequest(request)) return false;
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

        if (!_bufferReady || !_tempObject || _contentLength == 0) {
            request->send(400, F("text/plain"), F("Invalid request body"));
            return;
        }

        // Parse JSON with error handling
        gson::Parser parser;
        if (!parser.parse(reinterpret_cast<char*>(_tempObject.get()), _contentLength) || parser.hasError()) {
            request->send(400, F("text/plain"), F("Invalid JSON"));
            cleanup();
            return;
        }

        gson::Entry jsonRoot = parser.get(0);
        _onRequest(request, jsonRoot);
        cleanup();
    }

    bool isRequestHandlerTrivial() override final { return !_onRequest; }

private:
    void cleanup() noexcept {
        _bufferReady = false;
        _tempObject.reset();
        _tempObjectSize = 0;
    }
};

// ---------------------------
// Improved AsyncJsonStreamCallback
// ---------------------------
class AsyncJsonStreamCallback : public AsyncJsonHandlerBase {
public:
    using JsonStreamHandlerFunction = std::function<void(AsyncWebServerRequest*, gson::string&)>;

private:
    JsonStreamHandlerFunction _onJsonStreamRequest;
    AsyncWebServerRequest* _currentRequest;
    size_t _processIndex;

#ifdef ESP8266
    Ticker _nextChunkTimer;
#elif defined(ESP32)
    esp_timer_handle_t _chunkTimer;
    bool _timerInitialized;
#endif

    void cleanup() noexcept {
        _currentRequest = nullptr;
        _processIndex = 0;
        _bufferReady = false;
        _tempObject.reset();
        _tempObjectSize = 0;

#ifdef ESP8266
        _nextChunkTimer.detach();
#elif defined(ESP32)
        if (_timerInitialized && _chunkTimer) {
            esp_timer_stop(_chunkTimer);
            _timerInitialized = false;
        }
#endif
    }

#if defined(ESP8266) || defined(ESP32)
    void processNextChunk() {
        if (!_currentRequest || !_tempObject || _processIndex >= _tempObjectSize) {
            cleanup();
            return;
        }

        const size_t chunk_size = std::min((size_t)CHUNK_OBJ_SIZE, _tempObjectSize - _processIndex);
        if (chunk_size == 0) {
            cleanup();
            return;
        }

        // Use malloc for ESP8266/ESP32 compatibility
        char* chunk_data = static_cast<char*>(malloc(chunk_size));
        if (!chunk_data) {
            // Handle memory allocation failure
            if (_currentRequest) {
                _currentRequest->send(500, F("text/plain"), F("Memory allocation failed"));
            }
            cleanup();
            return;
        }

        memcpy(chunk_data, _tempObject.get() + _processIndex, chunk_size);

        gson::string rawJson;
        rawJson.addTextRaw(chunk_data, chunk_size);

        // Free the chunk memory immediately after use
        free(chunk_data);

        _onJsonStreamRequest(_currentRequest, rawJson);
        _processIndex += chunk_size;

        if (_processIndex < _tempObjectSize) {
            scheduleNextChunk();
        } else {
            cleanup();
        }
    }

    void scheduleNextChunk() {
#ifdef ESP8266
        _nextChunkTimer.once_ms(CHUNK_PROCESS_PERIOD_MS, [this]() { processNextChunk(); });
#elif defined(ESP32)
        if (_timerInitialized) {
            esp_timer_start_once(_chunkTimer, CHUNK_PROCESS_PERIOD_MS * 1000);
        }
#endif
    }

#ifdef ESP32
    static void timerCallback(void* arg) {
        if (arg) {
            static_cast<AsyncJsonStreamCallback*>(arg)->processNextChunk();
        }
    }

    bool initializeTimer() noexcept {
        if (_timerInitialized) return true;

        const esp_timer_create_args_t timer_args = {
            .callback = &timerCallback,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "json_chunk_timer",
            .skip_unhandled_events = false
        };

        esp_err_t result = esp_timer_create(&timer_args, &_chunkTimer);
        if (result == ESP_OK) {
            _timerInitialized = true;
            return true;
        }
        return false;
    }
#endif // ESP32
#endif // ESP8266 || ESP32

public:
    AsyncJsonStreamCallback(const String& uri, JsonStreamHandlerFunction onRequest) noexcept
        : AsyncJsonHandlerBase(uri), _onJsonStreamRequest(std::move(onRequest)), 
          _currentRequest(nullptr), _processIndex(0) {
#ifdef ESP32
        _chunkTimer = nullptr;
        _timerInitialized = false;
#endif
    }

    ~AsyncJsonStreamCallback() {
        cleanup();
#ifdef ESP32
        if (_timerInitialized && _chunkTimer) {
            esp_timer_delete(_chunkTimer);
        }
#endif
    }

    void onRequest(JsonStreamHandlerFunction fn) { _onJsonStreamRequest = std::move(fn); }

    bool canHandle(AsyncWebServerRequest* request) override final {
        if (!_onJsonStreamRequest || !validateRequest(request)) return false;
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

        if (!_bufferReady || !_tempObject || _tempObjectSize == 0) {
            request->send(400, F("text/plain"), F("Invalid request body"));
            return;
        }

        _currentRequest = request;
        _processIndex = 0;

#ifdef ESP8266
        processNextChunk();
#elif defined(ESP32)
        if (!initializeTimer()) {
            request->send(500, F("text/plain"), F("Timer initialization failed"));
            cleanup();
            return;
        }
        processNextChunk();
#else
        // Fallback for other platforms - process entire payload
        gson::string rawJson;
        if (rawJson.addTextRaw(reinterpret_cast<char*>(_tempObject.get()), _tempObjectSize)) {
            _onJsonStreamRequest(_currentRequest, rawJson);
        } else {
            request->send(500, F("text/plain"), F("Memory allocation failed"));
        }
        cleanup();
#endif
    }

    bool isRequestHandlerTrivial() override final { return !_onJsonStreamRequest; }
};

// ---------------------------
// Type aliases for compatibility
// ---------------------------
using JsonRequestHandlerFunction = AsyncCallbackJsonWebHandler::JsonRequestHandlerFunction;
using JsonStreamHandlerFunction = AsyncJsonStreamCallback::JsonStreamHandlerFunction;

#endif // ASYNC_JSON_H_
