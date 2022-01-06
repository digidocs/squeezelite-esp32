// Copyright (c) Kuba Szczodrzyński 2021-12-21.

#include "HTTPClient.h"
#include "TCPSocket.h"

using namespace bell;

struct HTTPClient::HTTPResponse *HTTPClient::execute(const struct HTTPRequest &request) {
	auto *response = new HTTPResponse();
	auto *url = request.url.c_str();
	HTTPClient::executeImpl(request, url, response);
	return response;
}

void HTTPClient::executeImpl(const struct HTTPRequest &request, const char *url, struct HTTPResponse *&response) {
	bool https = url[4] == 's';
	uint16_t port = https ? 443 : 80;
	auto *hostname = url + (https ? 8 : 7);
	auto *hostnameEnd = strchr(hostname, ':');
	auto *path = strchr(hostname, '/');
	if (hostnameEnd == nullptr) {
		hostnameEnd = path;
	} else {
		port = strtol(hostnameEnd + 1, nullptr, 10);
	}
	auto hostnameStr = std::string(hostname, (const char *)hostnameEnd);

	if (https) {
		response->socket = std::make_shared<TLSSocket>();
	} else {
		response->socket = std::make_shared<TCPSocket>();
	}
	response->socket->open(hostnameStr, port);

	const char *endl = "\r\n";
	std::stringstream stream;
	switch (request.method) {
		case HTTPMethod::GET:
			stream << "GET ";
			break;
		case HTTPMethod::POST:
			stream << "POST ";
			break;
	}
	stream << path << " HTTP/1.1" << endl;
	stream << "Host: " << hostnameStr << ":" << port << endl;
	stream << "Accept: */*" << endl;
	if (!request.body.empty()) {
		stream << "Content-Type: " << request.contentType << endl;
		stream << "Content-Length: " << request.body.size() << endl;
	}
	for (const auto &header : request.headers) {
		stream << header.first << ": " << header.second << endl;
	}
	stream << endl;
	stream << request.body;
	std::string data = stream.str();

	size_t len = response->socket->write((uint8_t *)data.c_str(), data.size());
	if (len != data.size()) {
		response->close();
		BELL_LOG(error, "http", "Writing failed: wrote %d of %d bytes", len, data.size());
		free(response);
		response = nullptr;
		return;
	}

	response->readHeaders();

	if (response->isRedirect && (request.maxRedirects < 0 || response->redirectCount < request.maxRedirects)) {
		response->redirectCount++;
		response->close(); // close the previous socket
		HTTPClient::executeImpl(request, response->location.c_str(), response);
	}
}

bool HTTPClient::readHeader(const char *&header, const char *name) {
	size_t len = strlen(name);
	if (strncasecmp(header, name, len) == 0) {
		header += len;
		while (*header == ' ')
			header++;
		return true;
	}
	return false;
}

size_t HTTPClient::HTTPResponse::readRaw(char *dst) {
	size_t len = this->socket->read((uint8_t *)dst, BUF_SIZE);
	//	BELL_LOG(debug, "http", "Read %d bytes", len);
	this->bodyRead += len; // after reading headers this gets overwritten
	dst[len] = '\0';
	return len;
}

void HTTPClient::HTTPResponse::readHeaders() {
	size_t len;
	char *line, *lineEnd;
	bool complete = false;
	std::string lineBuf;

	if (this->buf == nullptr) { // allocate a buffer
		this->buf = static_cast<char *>(malloc(BUF_SIZE + 1));
		this->bufPtr = this->buf;
	}

	// reset everything after a redirect
	this->statusCode = 0;
	this->contentLength = 0;
	this->isChunked = false;
	this->isGzip = false;
	this->isComplete = false;
	this->isRedirect = false;
	this->isStreaming = false;
	do {
		len = this->readRaw(this->buf);
		line = this->buf;
		do {
			lineEnd = strstr(line, "\r\n");
			if (!lineEnd) {
				lineBuf += std::string(line, this->buf + len);
				break;
			}
			lineBuf += std::string(line, lineEnd);
			if (lineBuf.empty()) {
				complete = true;
				// if body is present in buf, move the reading pointer
				if (lineEnd + 2 < this->buf + len) {
					this->bufPtr = lineEnd + 2;
					this->bufRemaining = len - (this->bufPtr - this->buf);
					this->bodyRead = this->bufRemaining;
					this->isStreaming =
						!this->isComplete && !this->contentLength && (len < BUF_SIZE || this->socket->poll() == 0);
				}
				break;
			}

			auto *header = lineBuf.c_str();
			if (strncmp(header, "HTTP/", 5) == 0) {
				header += 9; // skip "1.1 "
				this->statusCode = strtol(header, nullptr, 10);
			} else if (readHeader(header, "content-type:")) {
				this->contentType = std::string(header);
			} else if (readHeader(header, "content-length:")) {
				this->contentLength = strtol(header, nullptr, 10);
				if (!this->contentLength)
					this->isComplete = true; // forbid reading of the body
			} else if (readHeader(header, "transfer-encoding:")) {
				this->isChunked = strncmp(header, "chunked", 7) == 0;
			} else if (readHeader(header, "location:")) {
				this->isRedirect = true;
				this->location = std::string(header);
			} else {
				char *colonPtr = (char*) strchr(header, ':');
				if (colonPtr) {
					auto *valuePtr = colonPtr + 1;
					while (*valuePtr == ' ')
						valuePtr++;
					*colonPtr = '\0';
					for (auto *p = (char *)header; *p; ++p) // convert header name to lower case
						*p = (char)tolower(*p);
					this->headers[std::string(header)] = std::string(valuePtr);
				}
			}

			lineBuf.clear();
			line = lineEnd + 2; // skip \r\n
		} while (true);
	} while (!complete);
}

bool HTTPClient::HTTPResponse::skip(size_t len, bool dontRead) {
	size_t skip = 0;
	if (len > bufRemaining) {
		skip = len - bufRemaining;
		len = bufRemaining;
	}
	bufRemaining -= len;
	bufPtr += len;
	if (!bufRemaining && !dontRead) { // don't read more data after a chunk's \r\n
		if (isComplete || (contentLength && bodyRead >= contentLength && !chunkRemaining)) {
			isComplete = true;
			return false;
		}
		bufRemaining = this->readRaw(this->buf);
		if (!bufRemaining)
			return false; // no more data - shouldn't happen for valid responses
		bufPtr = this->buf + skip;
		bufRemaining -= skip;
		if (!contentLength && bufRemaining < BUF_SIZE) {
			// no content length set and the TCP buffer is not yielding more data, yet
			isStreaming = true;
		}
	}
	return true;
}

size_t HTTPClient::HTTPResponse::read(char *dst, size_t toRead) {
	if (isComplete) {
		// end of chunked stream was found OR complete body was read
		dst[0] = '\0';
		return 0;
	}
	auto *dstStart = dst;
	size_t read = 0;
	while (toRead) { // this loop ends after original toRead
		skip(0);	 // ensure the buffer contains data, wait if necessary
		if (isChunked && !chunkRemaining) {
			if (*bufPtr == '0') { // all chunks were read *and emitted*
				isComplete = true;
				break;
			}
			auto *endPtr = bufPtr;
			if (strchr(bufPtr, '\r') == nullptr) {						// buf doesn't contain complete chunk size
				auto size = std::string(bufPtr, bufPtr + bufRemaining); // take the rest of the buffer
				if (!skip(bufRemaining))								// skip the rest, read another buf
					break;												// -> no more data
				endPtr = strchr(bufPtr, '\r');							// find the end of the actual number
				if (endPtr == nullptr)									// something's wrong
					break;												//  - give up
				size += std::string(bufPtr, endPtr);					// append the newly read size
				chunkRemaining = std::stoul(size, nullptr, 16);			// read the hex size
			} else {
				chunkRemaining = strtol(bufPtr, &endPtr, 16); // read the hex size
			}
			if (!skip(endPtr - bufPtr + 2)) // skip the size and \r\n
				break;						// -> no more data, break out of main loop
		} else if (contentLength && !chunkRemaining) {
			chunkRemaining = contentLength;
		}

		while (chunkRemaining && toRead) {
			size_t count = std::min(toRead, std::min(bufRemaining, chunkRemaining));
			strncpy(dst, bufPtr, count);
			dst += count;			 // move the dst pointer
			read += count;			 // increment read counter
			chunkRemaining -= count; // decrease chunk remaining size
			toRead -= count;		 // decrease local remaining size
			if (!skip(count)) {		 // eat some buffer
				toRead = 0;			 // -> no more data, break out of main loop
				break;
			}
			if (isChunked && !chunkRemaining && !skip(2, isStreaming)) // skip the \r\n for chunked encoding
				toRead = 0;											   // -> no more data, break out of main loop
		}
		if (isStreaming && !bufRemaining) { // stream with no buffer available, just yield the current chunk
			break;
		}
	}
	if (!isChunked && contentLength && !chunkRemaining)
		isComplete = true;
	// BELL_LOG(debug, "http", "Read %d of %d bytes", bodyRead, contentLength);
	dstStart[read] = '\0';
	return read;
}

std::string HTTPClient::HTTPResponse::readToString() {
	if (this->contentLength) {
		std::string result(this->contentLength, '\0');
		this->read(result.data(), this->contentLength);
		this->close();
		return result;
	}
	std::string result;
	char buffer[BUF_SIZE];
	size_t len;
	do {
		len = this->read(buffer, BUF_SIZE);
		result += std::string(buffer);
	} while (len);
	this->close();
	return result;
}