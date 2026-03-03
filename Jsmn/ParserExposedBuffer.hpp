#ifndef JSMN_PARSEREXPOSEDBUFFER_HPP
#define JSMN_PARSEREXPOSEDBUFFER_HPP

#include<cstddef>
#include<functional>
#include<memory>
#include<stdexcept>
#include<string>
#include<utility>
#include<vector>
#include"Jsmn/Object.hpp"
#include"Jsmn/ParseError.hpp"

namespace Jsmn {

/** class Jsmn::ParserExposedBuffer
 *
 * A stateful jsmn-based parser, but with an exposed buffer
 * interface.
 * See Jsmn::Parser.
 *
 * Load data into the buffer using `load_buffer`, then
 * perform parsing of whatever is in the buffer in
 * `parse_buffer`.
 */
class ParserExposedBuffer {
private:
	class Impl;
	std::unique_ptr<Impl> pimpl;

public:
	ParserExposedBuffer();
	~ParserExposedBuffer();

	ParserExposedBuffer(ParserExposedBuffer const&) =delete;
	ParserExposedBuffer(ParserExposedBuffer&&) =delete;
	
	/** Reserve the given capacity in the buffer, then
	 * call the function with a pointer to the reserved
	 * capacity.
	 * The function should then return the actual number
	 * of bytes loaded.
	 */
	void load_buffer( std::size_t reserve
			, std::function<std::size_t(char*)> f
			);

	/** Check if there is data waiting in the buffer
	 * to be parsed.  */
	bool can_parse_buffer() const;

	/** Actually parse the data waiting in the buffer,
	 * returning the parsed JSON datums.
	 * If there are partial JSON datums, they will be
	 * retained in the buffer.
	 * After this call, can_parse_buffer() will return
	 * false until load_buffer is called and actual
	 * data is loaded in the load_buffer call (i.e.
	 * the function given to load_buffer returns non-0).
	 */
	std::vector<Jsmn::Object> parse_buffer();
};

}

#endif /* !defined(JSMN_PARSEREXPOSEDBUFFER_HPP) */
