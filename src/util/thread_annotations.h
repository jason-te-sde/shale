#ifndef SHALE_UTIL_THREAD_ANNOTATIONS_H_
#define SHALE_UTIL_THREAD_ANNOTATIONS_H_

// Macros for Clang's thread-safety analysis (-Wthread-safety). Under Clang they expand to the
// attributes the analysis reads; under every other compiler they expand to nothing, so the same
// source builds everywhere and only Clang checks it. See
// https://clang.llvm.org/docs/ThreadSafetyAnalysis.html for what each attribute means.

#if defined(__clang__)
#define SHALE_THREAD_ANNOTATION_(x) __attribute__((x))
#else
#define SHALE_THREAD_ANNOTATION_(x)
#endif

/// Marks a class as a lockable capability, such as a mutex.
#define SHALE_CAPABILITY(x) SHALE_THREAD_ANNOTATION_(capability(x))

/// Marks a class whose constructor acquires and whose destructor releases a capability.
#define SHALE_SCOPED_CAPABILITY SHALE_THREAD_ANNOTATION_(scoped_lockable)

/// Marks a data member that may be read or written only while holding the capability `x`.
#define SHALE_GUARDED_BY(x) SHALE_THREAD_ANNOTATION_(guarded_by(x))

/// Marks a pointer member whose pointee may be accessed only while holding `x`.
#define SHALE_PT_GUARDED_BY(x) SHALE_THREAD_ANNOTATION_(pt_guarded_by(x))

/// Marks a function that acquires the given capabilities (this one, if none are listed).
#define SHALE_ACQUIRE(...) SHALE_THREAD_ANNOTATION_(acquire_capability(__VA_ARGS__))

/// Marks a function that releases the given capabilities (this one, if none are listed).
#define SHALE_RELEASE(...) SHALE_THREAD_ANNOTATION_(release_capability(__VA_ARGS__))

/// Marks a function that may be called only while the caller holds the given capabilities.
#define SHALE_REQUIRES(...) SHALE_THREAD_ANNOTATION_(requires_capability(__VA_ARGS__))

/// Marks a function that may be called only while the caller does not hold the given capabilities.
#define SHALE_EXCLUDES(...) SHALE_THREAD_ANNOTATION_(locks_excluded(__VA_ARGS__))

/// Marks a function that checks at run time that `x` is held, and aborts if it is not.
#define SHALE_ASSERT_CAPABILITY(x) SHALE_THREAD_ANNOTATION_(assert_capability(x))

/// Turns the analysis off inside one function body, for code that handles the raw primitive.
#define SHALE_NO_THREAD_SAFETY_ANALYSIS SHALE_THREAD_ANNOTATION_(no_thread_safety_analysis)

#endif  // SHALE_UTIL_THREAD_ANNOTATIONS_H_
