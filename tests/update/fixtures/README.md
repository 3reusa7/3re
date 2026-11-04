# Update test fixtures

`signed.txt.minisig` was made by `tools/release/minisign.mjs sign` (Node's built-in Ed25519 and BLAKE2b) with a
throw-away key whose secret was deleted right after signing; `node-test.pub` is its public key. The C++
verifier (`apps/update/minisign.cpp`, Monocypher) must accept it and reject any change
(`tests/update/test_update_core.cpp`). This key is not trusted by 3re for anything.
