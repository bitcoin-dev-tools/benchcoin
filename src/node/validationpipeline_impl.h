// Copyright (c) 2026-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_VALIDATIONPIPELINE_IMPL_H
#define BITCOIN_NODE_VALIDATIONPIPELINE_IMPL_H

#include <node/validationpipeline.h>

#include <memory>

class ChainstateManager;

namespace node {

std::unique_ptr<ValidationPipeline> MakeValidationPipeline(ChainstateManager& chainman);

} // namespace node

#endif // BITCOIN_NODE_VALIDATIONPIPELINE_IMPL_H
