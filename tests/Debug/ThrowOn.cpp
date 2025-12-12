//
// Created by admin on 2025/12/12.
//

#include "ThrowOnAssign.h"
#include "ThrowOnCtor.h"

std::atomic<int> ThrowOnAssign::assignCount{ 0 };
int              ThrowOnAssign::throwOnAssign = -1;
std::atomic<int> ThrowOnCtor::liveCount{ 0 };
std::atomic<int> ThrowOnCtor::ctorCount{ 0 };
std::atomic<int> ThrowOnCtor::dtorCount{ 0 };
std::atomic<int> ThrowOnCtor::copyCount{ 0 };
std::atomic<int> ThrowOnCtor::moveCount{ 0 };
int              ThrowOnCtor::throwOnCtor = -1;
