/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef MU_PMWASM_PMCONVERTER_H
#define MU_PMWASM_PMCONVERTER_H

#include "modularity/imodulesetup.h"
#include "modularity/ioc.h"

namespace mu::pmwasm {
//! Module setup for the headless WebAssembly converter. It captures the IoC
//! context at app start so the Embind free function pmConvert() (which runs
//! outside the module system) can create an EngravingProject against the
//! registered engraving/mei services.
class PmWasmModule : public muse::modularity::IModuleSetup
{
public:
    explicit PmWasmModule(const muse::modularity::ContextPtr& iocCtx);

    std::string moduleName() const override;
    void onStartApp() override;

private:
    muse::modularity::ContextPtr m_iocCtx;
};

//! The IoC context captured at app start (null until then).
const muse::modularity::ContextPtr& pmIocContext();
}

#endif // MU_PMWASM_PMCONVERTER_H
