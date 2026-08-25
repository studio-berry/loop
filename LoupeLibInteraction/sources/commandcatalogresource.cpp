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

#include "commandcatalogresource_p.h"

void pdfinteraction::ensureCommandCatalogResource()
{
    // RCC emits qInitResources_* at global scope. Namespace members and local
    // scopes still resolve unqualified names inside pdfinteraction, so call the
    // global initializer explicitly.
    extern int qInitResources_commandcatalog();
    (void)qInitResources_commandcatalog();
}
