#ifndef UTIL_BACKTRACE_EXCEPTION_HPP
#define UTIL_BACKTRACE_EXCEPTION_HPP

#ifdef HAVE_CONFIG_H
# include"config.h"
#endif

#if !defined(ENABLE_EXCEPTION_BACKTRACE) || !ENABLE_EXCEPTION_BACKTRACE || defined(USE_VALGRIND)
// If backtraces are disabled (or we're running under valgrind), use a no-op wrapper.

#include <utility>

namespace Util {

/** class Util::BacktraceException<E>
 *
 * @brief A do-nothing wrapper when backtraces are disabled.
 */

template <typename T>
class BacktraceException : public T {
public:
    template <typename... Args>
    BacktraceException(Args&&... args)
        : T(std::forward<Args>(args)...) {}

    const char* what() const noexcept override {
        return T::what();
    }
};

} // namespace Util

#else // ENABLE_EXCEPTION_BACKTRACE in force and not running under valgrind

#include <array>
#include <execinfo.h>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

extern std::string g_argv0;

#define UNW_LOCAL_ONLY
#include <libunwind.h>

namespace Util {

/** class Util::BacktraceException<E>
 *
 * @brief A wrapper for an exception E which additionally stores a
 * backtrace when it is constructed.  The backtrace formatting is
 * deferred until `what()` is called on the handled exception.
 */

struct PcloseDeleter {
    void operator()(FILE* fp) const {
        if (fp) {
            pclose(fp);
        }
    }
};

template <typename T>
class BacktraceException : public T {
public:
	template <typename... Args>
	BacktraceException(Args&&... args)
		: T(std::forward<Args>(args)...), full_message_(T::what()) {
		capture_backtrace();
	}

	const char* what() const noexcept override {
		if (!formatted_) {
			formatted_ = true;
			full_message_ = create_full_message(T::what());
		}
		return full_message_.c_str();
	}

private:
	static constexpr size_t MAX_FRAMES = 100;
	mutable bool formatted_ = false;
	mutable std::string full_message_;
	mutable std::vector<unw_word_t> backtrace_addresses_;
	mutable size_t stack_depth;

	void capture_backtrace() {
		unw_cursor_t cursor;
		unw_context_t context;
		unw_getcontext(&context);
		unw_init_local(&cursor, &context);

		while (unw_step(&cursor) > 0 && backtrace_addresses_.size() < MAX_FRAMES) {
			unw_word_t ip;
			unw_get_reg(&cursor, UNW_REG_IP, &ip);
			backtrace_addresses_.push_back(ip);
		}

		stack_depth = backtrace_addresses_.size();
	}

	std::string format_backtrace() const {
		// Create an array of pointers compatible with `backtrace_symbols`.
		std::vector<void*> pointers(backtrace_addresses_.size());
		for (size_t i = 0; i < backtrace_addresses_.size(); ++i) {
			pointers[i] = reinterpret_cast<void*>(backtrace_addresses_[i]);
		}

		// Use `backtrace_symbols` with the adapted pointers.
		char** symbols = backtrace_symbols(pointers.data(), stack_depth);
		std::ostringstream oss;
		for (size_t i = 0; i < stack_depth; ++i) {
			oss << '#' << std::left << std::setfill(' ') << std::setw(2) << i << ' ';
			auto line = addr2line(pointers[i]);
			if (line.find("??") != std::string::npos) {
				// If addr2line doesn't find a good
				// answer, use basic information
				// instead.
				oss << symbols[i] << std::endl;
			} else {
				oss << line;
			}
		}
		free(symbols);
		return oss.str();
	}

        std::string progname() const {
		// This variable is set by the CLBOSS mainline.
		if (!g_argv0.empty()) {
			return g_argv0;
		}

		// When libclboss is linked with the unit tests the
		// g_argv0 variable is not set and we need to use
		// built-in versions
#ifdef HAVE_GETPROGNAME
		return getprogname();
#else
		return program_invocation_name;
#endif
	}

	// Unfortunately there is no simple way to get a high quality
	// backtrace using in-process libraries.  Instead for now we
	// popen an addr2line process and use it's output.
	std::string addr2line(void* addr) const {
		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			 "addr2line -C -f -p -e %s %p", progname().c_str(), addr);

		std::array<char, 128> buffer;
		std::string result;
		std::unique_ptr<FILE, PcloseDeleter> pipe(popen(cmd, "r"));

		if (!pipe) {
			return " -- error: unable to open addr2line";
		}

		while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
			result += buffer.data();
		}

		return result;
	}

	std::string create_full_message(const std::string& message) const {
		std::ostringstream oss;
		oss << message << "\nBacktrace:\n" << format_backtrace();
		return oss.str();
	}
};

} // namespace Util

#endif // ENABLE_EXCEPTION_BACKTRACE

#endif /* UTIL_BACKTRACE_EXCEPTION_HPP */
