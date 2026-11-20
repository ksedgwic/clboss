#include"Boltz/Detail/NormalConnection.hpp"
#include"Boltz/Detail/node_auth_message.hpp"
#include"Boltz/EnvIF.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Json/Out.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<ctime>
#include<curl/curl.h>
#include<vector>

#ifdef HAVE_CONFIG_H
# include"config.h"
#endif

namespace {

/* Class to create a CURL easy handle for JSONRPC,
 * then execute the given API call.  */
class EasyHandle {
private:
	Jsmn::Parser parser;
	Jsmn::Object result;
	bool has_result;
	bool parse_failed;
	std::string parse_error;

	std::vector<char> errbuf;

	curl_slist* headers;
	CURL* curl;

	EasyHandle() {
		errbuf.resize(CURL_ERROR_SIZE);
		for (auto& b : errbuf)
			b = 0;
		has_result = false;
		parse_failed = false;
		headers = NULL;
		headers = curl_slist_append(headers
					   , "Content-Type: application/json"
					   );
		curl = curl_easy_init();
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
		curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, &errbuf[0]);
	}
	~EasyHandle() {
		curl_easy_cleanup(curl);
		curl_slist_free_all(headers);
	}

public:
	/* `body` is nullptr for a GET.  */
	static
	Jsmn::Object run( std::string const& proxy
			, std::string const& api
			, std::string const* body
			, std::vector<std::string> const& extra_headers
			, std::uint32_t request_timeout
			) {
		EasyHandle self;
		self.run_core( proxy, api, body, extra_headers
			     , request_timeout
			     );
		return std::move(self.result);
	}

private:
	static
	size_t write_cb_s(char* ptr, size_t size, size_t nmemb, void* vself) {
		assert(size == 1);
		return ((EasyHandle*)vself)->write_cb(ptr, nmemb);
	}
	size_t write_cb(char* ptr, size_t size) {
		if (parse_failed)
			return 0;
		try {
			auto str = std::string(ptr, size);
			auto results = parser.feed(str);
			if (results.size() > 0 && !has_result) {
				has_result = true;
				result = std::move(results[0]);
			}
		} catch (std::exception const& e) {
			parse_failed = true;
			parse_error = e.what();
			return 0;
		}
		return size;
	}
	void run_core( std::string const& proxy
		     , std::string const& api
		     , std::string const* body
		     , std::vector<std::string> const& extra_headers
		     , std::uint32_t request_timeout
		     ) {
		for (auto const& header : extra_headers)
			headers = curl_slist_append(headers, header.c_str());
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &write_cb_s);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
		/* From --libcurl output.  */
		curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
		curl_easy_setopt(curl, CURLOPT_URL, api.c_str());
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
		if (proxy != "") {
			curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
			curl_easy_setopt( curl, CURLOPT_PROXYTYPE
					, (long)CURLPROXY_SOCKS5
					);
		}
		/* The caller keeps it alive until this returns.  */
		if (body) {
			curl_easy_setopt( curl, CURLOPT_POSTFIELDS
					, body->c_str()
					);
			curl_easy_setopt( curl, CURLOPT_POSTFIELDSIZE_LARGE
					, (curl_off_t) body->size()
					);
		}
		curl_easy_setopt( curl, CURLOPT_USERAGENT
				, "clboss/" PACKAGE_VERSION
				);
		curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50L);
		curl_easy_setopt( curl, CURLOPT_HTTP_VERSION
				, (long) CURL_HTTP_VERSION_2TLS
				);
		curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1);
		/* Keepalive notices a dead peer; this notices a live
		 * one that sends nothing.  */
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long) request_timeout);

		try {
			auto ret = curl_easy_perform(curl);
			if (parse_failed) {
				throw Boltz::ApiError(std::string("Invalid JSON for ")
						      + api + ": "
						      + parse_error);
			}
			if (ret != 0) {
				auto msg = std::string(api)
					 + ": "
					 + std::string(curl_easy_strerror(ret))
					 + ": "
					 + std::string(&errbuf[0])
					 ;
				throw Boltz::ApiError(msg);
			}
		} catch (Boltz::ApiError const&) {
			throw;
		} catch (std::exception const& e) {
			auto msg = std::string("While accessing ")
				 + api
				 + ": "
				 + e.what();
			throw Boltz::ApiError(msg);
		}
		if (!has_result) {
			throw Boltz::ApiError(std::string("No JSON result for ")
					      + api);
		}
	}
};

}

namespace Boltz { namespace Detail {

class NormalConnection::Impl {
private:
	Ev::ThreadPool& threadpool;
	std::string api_base;
	std::string proxy;
	std::uint32_t request_timeout;
	Boltz::EnvIF* signer;

	/* Headers proving which node sent the request, or none
	 * without a signer or a signature.  */
	Ev::Io<std::vector<std::string>>
	sign( std::string const& method
	    , std::string const& api
	    , std::string const& body
	    ) {
		if (!signer)
			return Ev::lift(std::vector<std::string>());
		auto ts = std::uint64_t(std::time(nullptr));
		return signer->sign_message(
			node_auth_message(ts, method, api, body)
		).then([ts](Boltz::NodeSignature sig) {
			auto headers = std::vector<std::string>();
			if (sig.node_id != "" && sig.zbase != "") {
				headers.push_back("x-node-id: " + sig.node_id);
				headers.push_back( "x-node-timestamp: "
						 + std::to_string(ts)
						 );
				headers.push_back( "x-node-signature: "
						 + sig.zbase
						 );
			}
			return Ev::lift(std::move(headers));
		});
	}

public:
	Impl() =delete;
	Impl(Impl const&) =delete;
	Impl(Impl&&) =delete;

	Impl( Ev::ThreadPool& threadpool_
	    , std::string api_base_
	    , std::string proxy_
	    , std::uint32_t request_timeout_
	    , Boltz::EnvIF* signer_
	    ) : threadpool(threadpool_)
	      , api_base(std::move(api_base_))
	      , proxy(std::move(proxy_))
	      , request_timeout(request_timeout_)
	      , signer(signer_)
	      { }

	Ev::Io<Jsmn::Object>
	api(std::string api, std::unique_ptr<Json::Out> params) {
		/* Serialized once, so the bytes sent are the bytes
		 * signed.  */
		auto has_body = bool(params);
		auto body = std::make_shared<std::string>(
			has_body ? params->output() : std::string()
		);
		return sign( has_body ? "POST" : "GET", api, *body
		).then([this, api, body, has_body
		       ](std::vector<std::string> headers) {
			auto pheaders = std::make_shared<std::vector<std::string>>(
				std::move(headers)
			);
			return threadpool.background<Jsmn::Object>([ this
								   , api
								   , body
								   , has_body
								   , pheaders
								   ]() {
				return EasyHandle::run( proxy
						      , api_base + api
						      , has_body ? body.get() : nullptr
						      , *pheaders
						      , request_timeout
						      );
			});
		});
	}
};

NormalConnection::~NormalConnection() =default;
NormalConnection::NormalConnection(NormalConnection&&) =default;
NormalConnection::NormalConnection( Ev::ThreadPool& threadpool
				  , std::string api_base
				  , std::string proxy
				  , std::uint32_t request_timeout
				  , Boltz::EnvIF* signer
				  ) : pimpl(Util::make_unique<Impl>( threadpool
								   , std::move(api_base)
								   , std::move(proxy)
								   , request_timeout
								   , signer
								   ))
				    { }

Ev::Io<Jsmn::Object>
NormalConnection::api(std::string api, std::unique_ptr<Json::Out> params) {
	return pimpl->api(api, std::move(params));
}

}}
