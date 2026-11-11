#pragma once

// The version is project(VERSION ...) in CMakeLists.txt, passed in as a define.
// A copy written out here went stale once: 1.0.41 shipped calling itself 1.0.40,
// so every updated copy kept offering the same update. A release bumps
// CMakeLists.txt and Cargo.toml, and nothing else.
#ifndef LD_VERSION_STRING
#error "LD_VERSION_STRING comes from CMakeLists.txt"
#endif
