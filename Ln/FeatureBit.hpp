#ifndef LN_FEATUREBIT_HPP
#define LN_FEATUREBIT_HPP

#include<string>

namespace Ln {

/** Ln::feature_bit
 *
 * @brief determine whether the given feature bit is set in a
 * hex-encoded feature bitfield (BOLT #9 encoding: bit 0 is the
 * least-significant bit of the last byte).
 *
 * @desc returns false for malformed hex, or for bitfields too
 * short to contain the bit.
 */
bool feature_bit(std::string const& features_hex, unsigned int bit);

}

#endif /* !defined(LN_FEATUREBIT_HPP) */
