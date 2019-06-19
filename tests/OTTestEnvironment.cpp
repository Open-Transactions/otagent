// Copyright (c) 2018 The Open-Transactions developers
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "OTTestEnvironment.hpp"

#include "opentxs/opentxs.hpp"

void OTTestEnvironment::SetUp()
{
    opentxs::ArgList args{{OPENTXS_ARG_STORAGE_PLUGIN, {"mem"}}};
    opentxs::InitContext(args);
}

void OTTestEnvironment::TearDown() { opentxs::Cleanup(); }

OTTestEnvironment::~OTTestEnvironment() = default;
