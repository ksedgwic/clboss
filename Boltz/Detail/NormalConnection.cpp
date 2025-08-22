#include"Boltz/Detail/NormalConnection.hpp"
#include"Ev/Io.hpp"
#include"Ev/ThreadPool.hpp"
#include"Jsmn/Object.hpp"
#include"Jsmn/Parser.hpp"
#include"Json/Out.hpp"
#include"Util/make_unique.hpp"
#include<assert.h>
#include<curl/curl.h>

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

	std::vector<char> errbuf;

	curl_slist* headers;
	CURL* curl;

	EasyHandle() {
		errbuf.resize(CURL_ERROR_SIZE);
		for (auto& b : errbuf)
			b = 0;
		has_result = false;
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
	static
	Jsmn::Object run( std::string const& proxy
			, std::string const& api
			, std::unique_ptr<Json::Out> parms
			) {
		EasyHandle self;
		self.run_core(proxy, api, std::move(parms));
		return std::move(self.result);
	}

private:
	static
	size_t write_cb_s(char* ptr, size_t size, size_t nmemb, void* vself) {
		assert(size == 1);
		return ((EasyHandle*)vself)->write_cb(ptr, nmemb);
	}
	size_t write_cb(char* ptr, size_t size) {
		auto str = std::string(ptr, size);
		auto results = parser.feed(str);
		if (results.size() > 0 && !has_result) {
			has_result = true;
			result = std::move(results[0]);
		}
		return size;
	}
	void run_core( std::string const& proxy
		     , std::string const& api
		     , std::unique_ptr<Json::Out> parms
		     ) {

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
		/* Needs to survive until after curl_easy_perform.  */
		auto postfields = std::string();
		if (parms) {
			postfields = parms->output();
			curl_easy_setopt( curl, CURLOPT_POSTFIELDS
					, postfields.c_str()
					);
			curl_easy_setopt( curl, CURLOPT_POSTFIELDSIZE_LARGE
					, (curl_off_t) postfields.size()
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

		try {
			auto ret = curl_easy_perform(curl);
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
	}
};

}

namespace Boltz { namespace Detail {

class NormalConnection::Impl {
private:
	Ev::ThreadPool& threadpool;
	std::string api_base;
	std::string proxy;

public:
	Impl() =delete;
	Impl(Impl const&) =delete;
	Impl(Impl&&) =delete;

	Impl( Ev::ThreadPool& threadpool_
	    , std::string api_base_
	    , std::string proxy_
	    ) : threadpool(threadpool_)
	      , api_base(std::move(api_base_))
	      , proxy(std::move(proxy_))
	      { }

	Ev::Io<Jsmn::Object>
	api(std::string api, std::unique_ptr<Json::Out> params) {
		auto pparams = std::make_shared<std::unique_ptr<Json::Out>>(
			std::move(params)
		);
		return threadpool.background<Jsmn::Object>([ this
							   , api
							   , pparams
							   ]() {
			return EasyHandle::run( proxy
					      , api_base + api
					      , std::move(*pparams)
					      );
		});
	}
};

NormalConnection::~NormalConnection() =default;
NormalConnection::NormalConnection(NormalConnection&&) =default;
NormalConnection::NormalConnection( Ev::ThreadPool& threadpool
				  , std::string api_base
				  , std::string proxy
				  ) : pimpl(Util::make_unique<Impl>( threadpool
								   , std::move(api_base)
								   , std::move(proxy)
								   ))
				    { }

Ev::Io<Jsmn::Object>
NormalConnection::api(std::string api, std::unique_ptr<Json::Out> params) {
	return pimpl->api(api, std::move(params));
}

}}
