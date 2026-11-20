#include"Boltz/EnvIF.hpp"
#include"Ev/Io.hpp"

namespace Boltz {

Ev::Io<NodeSignature> EnvIF::sign_message(std::string) {
	return Ev::lift(NodeSignature());
}

}
