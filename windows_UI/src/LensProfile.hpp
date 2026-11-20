#pragma once

#include "io/raw_decoder.hpp"
#include <QString>

// Import a rectilinear RAW Adobe LCP for one photograph. The result is a
// self-contained edit (name, optical centre, normalized radial coefficients).
QString readLensProfile(const QString& path, const spk::io::RawMetadata& photo);
