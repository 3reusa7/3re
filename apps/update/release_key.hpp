#pragma once
// The one key 3re trusts for updates: tools/release/3re-update.pub (minisign, key id 6EA665394615A750).
// Its secret exists only in the repository's Actions secret THREERE_UPDATE_SIGNING_KEY and in the
// maintainer's offline backup. Replacing it means every installed copy needs a manual update once.
namespace threere::update {
inline constexpr const char* kReleasePublicKey = "RWRQpxVGOWWmbuzzkmTFAKgBdhiYsfwb8QX105v54s6hvDPzmHOn2CRy";
// owner/name of the GitHub repository whose Releases are checked (CMake THREERE_UPDATE_REPOSITORY;
// the release workflow sets it to the repository that builds the release).
#ifdef THREERE_UPDATE_REPOSITORY
inline constexpr const char* kReleaseRepository = THREERE_UPDATE_REPOSITORY;
#else
inline constexpr const char* kReleaseRepository = "3reusa7/3re";
#endif
}  // namespace threere::update
