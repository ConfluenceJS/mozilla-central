/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

// Original author: ekr@rtfm.com

#ifndef runnable_utils_h__
#define runnable_utils_h__

#include "nsThreadUtils.h"

// Abstract base class for all of our templates
namespace mozilla {

class runnable_args_base : public nsRunnable {
 public:

  NS_IMETHOD Run() = 0;
};


// The generated file contains four major function templates
// (in variants for arbitrary numbers of arguments up to 10,
// which is why it is machine generated). The four templates
// are:
//
// WrapRunnable(o, m, ...) -- wraps a member function m of an object ptr o 
// WrapRunnableRet(o, m, ..., r) -- wraps a member function m of an object ptr o
//                                  the function returns something that can
//                                  be assigned to *r
// WrapRunnableNM(f, ...) -- wraps a function f
// WrapRunnableNMRet(f, ..., r) -- wraps a function f that returns something
//                                 that can be assigned to *r
// 
// All of these template functions return a Runnable* which can be passed
// to Dispatch().
#include "runnable_utils_generated.h"

// Temporary hack. Really we want to have a template which will do this
#define RUN_ON_THREAD(t, r, h) ((t && (t != nsRefPtr<nsIThread>(do_GetCurrentThread()))) ? t->Dispatch(r, h) : r->Run())
}

#endif

