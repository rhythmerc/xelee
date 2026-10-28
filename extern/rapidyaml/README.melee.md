rapidyaml 0.15.2, the unmodified single-header amalgamation released as
rapidyaml-0.15.2.hpp at
https://github.com/biojppm/rapidyaml/releases/tag/v0.15.2
(sha256 6e86154a6433875031130d4e0a98eb0fc3e160cc8acd99634a889d7934319288),
renamed ryml_all.hpp. It bundles c4core.

MIT license (LICENSE.txt, also at the top of the header). Used to read the
game's text, resources/lang/<language>.yaml: src/pc/lang.cpp is the one file
that defines RYML_SINGLE_HDR_DEFINE_NOW and includes it.
