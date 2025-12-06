//
// Created by admin on 25-11-24.
//

#ifndef COMMON_H
#define COMMON_H


#ifndef HAKLE_CPP_VERSION
#if defined(_MSVC_LANG)
#define HAKLE_CPLUSPLUS _MSVC_LANG
#else
#define HAKLE_CPLUSPLUS __cplusplus
#endif

#if HAKLE_CPLUSPLUS >= 202002L
#define HAKLE_CPP_VERSION 20
#elif HAKLE_CPLUSPLUS >= 201703L
#define HAKLE_CPP_VERSION 17
#elif HAKLE_CPLUSPLUS >= 201402L
#define HAKLE_CPP_VERSION 14
#elif HAKLE_CPLUSPLUS >= 201103L
#define HAKLE_CPP_VERSION 11
#else
#define HAKLE_CPP_VERSION 0
#error "This library requires C++11 or later"
#endif

#endif


#if HAKLE_CPP_VERSION >= 17
#define CONSTEXPR_IF constexpr
#else
#define CONSTEXPR_IF
#endif



#endif //COMMON_H
