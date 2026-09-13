#pragma once

// The one place the version is written. BKC_OnPlugInInfo (what Becky! shows)
// and the VERSIONINFO resource (what Explorer shows) both read it from here,
// and the bundled README states the same string; change it here and they
// cannot drift apart.
//
// Semantic versioning: MAJOR for a change that breaks what a user already
// has (config.ini keys, the learned-data files, the key store), MINOR for a
// feature, PATCH for a fix. A "-rc.N" suffix marks a release candidate: the
// same thing 1.0.0 will be unless a problem is found, with the compatibility
// promise already in force - trial users' settings and learned data carry
// over to the release.
#define BKCLAUDE_VERSION_MAJOR  1
#define BKCLAUDE_VERSION_MINOR  0
#define BKCLAUDE_VERSION_PATCH  0
#define BKCLAUDE_VERSION_STRING "1.0.0-rc.1"
// 1 while the string carries a pre-release suffix; sets VS_FF_PRERELEASE on
// the DLL. Set to 0 in the same edit that removes the suffix.
#define BKCLAUDE_PRERELEASE     1

// Usable inside the .rc, which cannot compute the string from the numbers.
#define BKCLAUDE_VERSION_RC_NUMERIC \
	BKCLAUDE_VERSION_MAJOR,BKCLAUDE_VERSION_MINOR,BKCLAUDE_VERSION_PATCH,0
