// MIT License
//
// Copyright (c) 2018-2025 Jakub Melka and Contributors
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

#ifndef COMMANDCATALOGRESOURCE_P_H
#define COMMANDCATALOGRESOURCE_P_H

namespace pdfinteraction
{

/// Pulls the static-library resource initializer into any consumer that uses
/// CommandCatalog. Without this anchor, a linker may discard the AUTORCC
/// object and leave the catalog's embedded contract unavailable.
void ensureCommandCatalogResource();

}   // namespace pdfinteraction

#endif   // COMMANDCATALOGRESOURCE_P_H
