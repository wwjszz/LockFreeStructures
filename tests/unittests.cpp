// ©2013-2015 Cameron Desrochers
// Unit tests for moodycamel::ReaderWriterQueue

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "ReaderWriterQueue/readerwriterqueue.h"
#include "tests/simplethread.h"
#include "unittests.h"

using namespace hakle;

// *NOT* thread-safe
struct Foo {
    Foo() : copied( false ) { id = _id()++; }
    Foo( Foo const& other ) : id( other.id ), copied( true ) {}
    Foo( Foo&& other ) : id( other.id ), copied( other.copied ) { other.copied = true; }
    Foo& operator=( Foo&& other ) {
        verify();
        id = other.id, copied = other.copied;
        other.copied = true;
        return *this;
    }
    ~Foo() { verify(); }

private:
    void verify() {
        if ( copied )
            return;
        if ( id != _last_destroyed_id() + 1 ) {
            _destroyed_in_order() = false;
        }
        _last_destroyed_id() = id;
        ++_destroy_count();
    }

public:
    static void reset() {
        _destroy_count()      = 0;
        _id()                 = 0;
        _destroyed_in_order() = true;
        _last_destroyed_id()  = -1;
    }
    static int  destroy_count() { return _destroy_count(); }
    static bool destroyed_in_order() { return _destroyed_in_order(); }

private:
    static int& _destroy_count() {
        static int c = 0;
        return c;
    }
    static int& _id() {
        static int i = 0;
        return i;
    }
    static bool& _destroyed_in_order() {
        static bool d = true;
        return d;
    }
    static int& _last_destroyed_id() {
        static int i = -1;
        return i;
    }

    int  id;
    bool copied;
};

class UniquePtrWrapper {
public:
    UniquePtrWrapper() = default;
    UniquePtrWrapper( std::unique_ptr<int> p ) : m_p( std::move( p ) ) {}
    int                   get_value() const { return *m_p; }
    std::unique_ptr<int>& get_ptr() { return m_p; }

private:
    std::unique_ptr<int> m_p;
};
/// Extracted from private static method of ReaderWriterQueue
static size_t ceilToPow2( size_t x ) {
    // From http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
    --x;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    for ( size_t i = 1; i < sizeof( size_t ); i <<= 1 ) {
        x |= x >> ( i << 3 );
    }
    ++x;
    return x;
}

class ReaderWriterQueueTests : public TestClass<ReaderWriterQueueTests> {
public:
    ReaderWriterQueueTests() {
        REGISTER_TEST( create_empty_queue );
        REGISTER_TEST( Enqueue_one );
        REGISTER_TEST( Enqueue_many );
        REGISTER_TEST( nonempty_destroy );
        REGISTER_TEST( TryEnqueue );
        REGISTER_TEST( TryDequeue );
        REGISTER_TEST( Peek );
        REGISTER_TEST( Pop );
        REGISTER_TEST( SizeApprox );
        REGISTER_TEST( MaxCapacity );
        REGISTER_TEST( threaded );
        REGISTER_TEST( blocking );
        REGISTER_TEST( vector );

        REGISTER_TEST( Emplace );
        REGISTER_TEST( TryEnqueue_fail_workaround );
        REGISTER_TEST( TryEmplace_fail );
    }

    bool create_empty_queue() {
        {
            ReaderWriterQueue<int> q;
        }

        {
            ReaderWriterQueue<int> q( 1234 );
        }

        return true;
    }

    bool Enqueue_one() {
        int item;

        {
            item = 0;
            ReaderWriterQueue<int> q( 1 );
            q.Enqueue( 12345 );
            ASSERT_OR_FAIL( q.TryDequeue( item ) );
            ASSERT_OR_FAIL( item == 12345 );
        }

        {
            item = 0;
            ReaderWriterQueue<int> q( 1 );
            ASSERT_OR_FAIL( q.TryEnqueue( 12345 ) );
            ASSERT_OR_FAIL( q.TryDequeue( item ) );
            ASSERT_OR_FAIL( item == 12345 );
        }

        return true;
    }

    bool Enqueue_many() {
        int item = -1;

        {
            ReaderWriterQueue<int> q( 100 );
            for ( int i = 0; i != 100; ++i ) {
                q.Enqueue( i );
            }

            for ( int i = 0; i != 100; ++i ) {
                ASSERT_OR_FAIL( q.TryDequeue( item ) );
                ASSERT_OR_FAIL( item == i );
            }
        }

        {
            ReaderWriterQueue<int> q( 100 );
            for ( int i = 0; i != 1200; ++i ) {
                q.Enqueue( i );
            }

            for ( int i = 0; i != 1200; ++i ) {
                ASSERT_OR_FAIL( q.TryDequeue( item ) );
                ASSERT_OR_FAIL( item == i );
            }
        }

        return true;
    }

    bool nonempty_destroy() {
        // Some elements at beginning
        Foo::reset();
        {
            ReaderWriterQueue<Foo> q( 31 );
            for ( int i = 0; i != 10; ++i ) {
                q.Enqueue( Foo() );
            }
            ASSERT_OR_FAIL( Foo::destroy_count() == 0 );
        }
        ASSERT_OR_FAIL( Foo::destroy_count() == 10 );
        ASSERT_OR_FAIL( Foo::destroyed_in_order() );

        // Entire block
        Foo::reset();
        {
            ReaderWriterQueue<Foo> q( 31 );
            for ( int i = 0; i != 31; ++i ) {
                q.Enqueue( Foo() );
            }
            ASSERT_OR_FAIL( Foo::destroy_count() == 0 );
        }
        ASSERT_OR_FAIL( Foo::destroy_count() == 31 );
        ASSERT_OR_FAIL( Foo::destroyed_in_order() );

        // Multiple blocks
        Foo::reset();
        {
            ReaderWriterQueue<Foo> q( 31 );
            for ( int i = 0; i != 94; ++i ) {
                q.Enqueue( Foo() );
            }
            ASSERT_OR_FAIL( Foo::destroy_count() == 0 );
        }
        ASSERT_OR_FAIL( Foo::destroy_count() == 94 );
        ASSERT_OR_FAIL( Foo::destroyed_in_order() );

        // Some elements in another block
        Foo::reset();
        {
            ReaderWriterQueue<Foo> q( 31 );
            Foo                    item;
            for ( int i = 0; i != 42; ++i ) {
                q.Enqueue( Foo() );
            }
            ASSERT_OR_FAIL( Foo::destroy_count() == 0 );
            for ( int i = 0; i != 31; ++i ) {
                ASSERT_OR_FAIL( q.TryDequeue( item ) );
            }
            ASSERT_OR_FAIL( Foo::destroy_count() == 31 );
        }
        ASSERT_OR_FAIL( Foo::destroy_count() == 43 );
        ASSERT_OR_FAIL( Foo::destroyed_in_order() );

        // Some elements in multiple blocks
        Foo::reset();
        {
            ReaderWriterQueue<Foo> q( 31 );
            Foo                    item;
            for ( int i = 0; i != 123; ++i ) {
                q.Enqueue( Foo() );
            }
            for ( int i = 0; i != 25; ++i ) {
                ASSERT_OR_FAIL( q.TryDequeue( item ) );
            }
            for ( int i = 0; i != 47; ++i ) {
                q.Enqueue( Foo() );
            }
            for ( int i = 0; i != 140; ++i ) {
                ASSERT_OR_FAIL( q.TryDequeue( item ) );
            }
            for ( int i = 0; i != 230; ++i ) {
                q.Enqueue( Foo() );
            }
            for ( int i = 0; i != 130; ++i ) {
                ASSERT_OR_FAIL( q.TryDequeue( item ) );
            }
            for ( int i = 0; i != 100; ++i ) {
                q.Enqueue( Foo() );
            }
        }
        ASSERT_OR_FAIL( Foo::destroy_count() == 501 );
        ASSERT_OR_FAIL( Foo::destroyed_in_order() );

        return true;
    }

    bool TryEnqueue() {
        ReaderWriterQueue<int> q( 31 );
        int                    item;
        int                    size = 0;

        for ( int i = 0; i < 10000; ++i ) {
            if ( ( rand() & 1 ) == 1 ) {
                bool result = q.TryEnqueue( i );
                if ( size == 31 ) {
                    ASSERT_OR_FAIL( !result );
                }
                else {
                    ASSERT_OR_FAIL( result );
                    ++size;
                }
            }
            else {
                bool result = q.TryDequeue( item );
                if ( size == 0 ) {
                    ASSERT_OR_FAIL( !result );
                }
                else {
                    ASSERT_OR_FAIL( result );
                    --size;
                }
            }
        }

        return true;
    }

    bool TryDequeue() {
        int item;

        {
            ReaderWriterQueue<int> q( 1 );
            ASSERT_OR_FAIL( !q.TryDequeue( item ) );
        }

        {
            ReaderWriterQueue<int, 2> q( 10 );
            ASSERT_OR_FAIL( !q.TryDequeue( item ) );
        }

        return true;
    }

    bool threaded() {
        WeakAtomic<int> result;
        result = 1;

        ReaderWriterQueue<int> q( 100 );
        SimpleThread           reader( [ & ]() {
            int item;
            int prevItem = -1;
            for ( int i = 0; i != 1000000; ++i ) {
                if ( q.TryDequeue( item ) ) {
                    if ( item <= prevItem ) {
                        result = 0;
                    }
                    prevItem = item;
                }
            }
        } );
        SimpleThread           writer( [ & ]() {
            for ( int i = 0; i != 1000000; ++i ) {
                if ( ( ( i >> 7 ) & 1 ) == 0 ) {
                    q.Enqueue( i );
                }
                else {
                    q.TryEnqueue( i );
                }
            }
        } );

        writer.join();
        reader.join();

        return result.Load() == 1 ? true : false;
    }

    bool Peek() {
        WeakAtomic<int> result;
        result = 1;

        ReaderWriterQueue<int> q( 100 );
        SimpleThread           reader( [ & ]() {
            int  item;
            int  prevItem = -1;
            int* Peeked;
            for ( int i = 0; i != 100000; ++i ) {
                Peeked = q.Peek();
                if ( Peeked != nullptr ) {
                    if ( q.TryDequeue( item ) ) {
                        if ( item <= prevItem || item != *Peeked ) {
                            result = 0;
                        }
                        prevItem = item;
                    }
                    else {
                        result = 0;
                    }
                }
            }
        } );
        SimpleThread           writer( [ & ]() {
            for ( int i = 0; i != 100000; ++i ) {
                if ( ( ( i >> 7 ) & 1 ) == 0 ) {
                    q.Enqueue( i );
                }
                else {
                    q.TryEnqueue( i );
                }
            }
        } );

        writer.join();
        reader.join();

        return result.Load() == 1 ? true : false;
    }

    bool Pop() {
        WeakAtomic<int> result;
        result = 1;

        ReaderWriterQueue<int> q( 100 );
        SimpleThread           reader( [ & ]() {
            int  item;
            int  prevItem = -1;
            int* Peeked;
            for ( int i = 0; i != 100000; ++i ) {
                Peeked = q.Peek();
                if ( Peeked != nullptr ) {
                    item = *Peeked;
                    if ( q.Pop() ) {
                        if ( item <= prevItem ) {
                            result = 0;
                        }
                        prevItem = item;
                    }
                    else {
                        result = 0;
                    }
                }
            }
        } );
        SimpleThread           writer( [ & ]() {
            for ( int i = 0; i != 100000; ++i ) {
                if ( ( ( i >> 7 ) & 1 ) == 0 ) {
                    q.Enqueue( i );
                }
                else {
                    // q.TryEnqueue(i);
                }
            }
        } );

        writer.join();
        reader.join();

        return result.Load() == 1 ? true : false;
    }

    bool SizeApprox() {
        WeakAtomic<int> result;
        WeakAtomic<int> front;
        WeakAtomic<int> tail;

        result = 1;
        front  = 0;
        tail   = 0;

        ReaderWriterQueue<int> q( 10 );
        SimpleThread           reader( [ & ]() {
            int item;
            for ( int i = 0; i != 100000; ++i ) {
                if ( q.TryDequeue( item ) ) {
                    std::atomic_thread_fence( std::memory_order_release );
                    front = front.Load() + 1;
                }
                int size = static_cast<int>( q.SizeApprox() );
                std::atomic_thread_fence( std::memory_order_acquire );
                int tail_  = tail.Load();
                int front_ = front.Load();
                if ( size > tail_ - front_ || size < 0 ) {
                    result = 0;
                }
            }
        } );
        SimpleThread           writer( [ & ]() {
            for ( int i = 0; i != 100000; ++i ) {
                tail = tail.Load() + 1;
                std::atomic_thread_fence( std::memory_order_release );
                q.Enqueue( i );
                int tail_  = tail.Load();
                int front_ = front.Load();
                std::atomic_thread_fence( std::memory_order_acquire );
                int size = static_cast<int>( q.SizeApprox() );
                if ( size > tail_ - front_ || size < 0 ) {
                    result = 0;
                }
            }
        } );

        writer.join();
        reader.join();

        return result.Load() == 1 ? true : false;
    }

    bool MaxCapacity() {
        {
            // this math for queue size estimation is only valid for q_size <= 256
            for ( size_t q_size = 2; q_size < 256; ++q_size ) {
                ReaderWriterQueue<size_t> q( q_size );
                ASSERT_OR_FAIL( q.MaxCapacity() == ceilToPow2( q_size + 1 ) - 1 );

                const size_t start_cap = q.MaxCapacity();
                for ( size_t i = 0; i < start_cap + 1; ++i )  // fill 1 past capacity to resize
                    q.Enqueue( i );
                ASSERT_OR_FAIL( q.MaxCapacity() == 3 * start_cap + 1 );
            }
        }
        return true;
    }

    bool vector() {
        {
            std::vector<ReaderWriterQueue<int>> queues;
            queues.push_back( ReaderWriterQueue<int>() );
            queues.emplace_back();

            queues[ 0 ].Enqueue( 1 );
            queues[ 1 ].Enqueue( 2 );
            std::swap( queues[ 0 ], queues[ 1 ] );

            int item;
            ASSERT_OR_FAIL( queues[ 0 ].TryDequeue( item ) );
            ASSERT_OR_FAIL( item == 2 );

            ASSERT_OR_FAIL( queues[ 1 ].TryDequeue( item ) );
            ASSERT_OR_FAIL( item == 1 );
        }

        return true;
    }

    bool blocking() {
        {
            BlockingReaderWriterQueue<int> q;
            int                            item;

            q.Enqueue( 123 );
            ASSERT_OR_FAIL( q.TryDequeue( item ) );
            ASSERT_OR_FAIL( item == 123 );
            ASSERT_OR_FAIL( q.SizeApprox() == 0 );

            q.Enqueue( 234 );
            ASSERT_OR_FAIL( q.SizeApprox() == 1 );
            ASSERT_OR_FAIL( *q.Peek() == 234 );
            ASSERT_OR_FAIL( *q.Peek() == 234 );
            ASSERT_OR_FAIL( q.Pop() );

            ASSERT_OR_FAIL( q.TryEnqueue( 345 ) );
            q.Dequeue( item );
            ASSERT_OR_FAIL( item == 345 );
            ASSERT_OR_FAIL( !q.Peek() );
            ASSERT_OR_FAIL( q.SizeApprox() == 0 );
            ASSERT_OR_FAIL( !q.TryDequeue( item ) );
        }

        WeakAtomic<int> result{};
        result = 1;

        {
            BlockingReaderWriterQueue<int> q( 100 );
            SimpleThread                   reader( [ & ]() {
                int item     = -1;
                int prevItem = -1;
                for ( int i = 0; i != 1000000; ++i ) {
                    q.Dequeue( item );
                    if ( item <= prevItem ) {
                        result = 0;
                    }
                    prevItem = item;
                }
            } );
            SimpleThread                   writer( [ & ]() {
                for ( int i = 0; i != 1000000; ++i ) {
                    q.Enqueue( i );
                }
            } );

            writer.join();
            reader.join();

            ASSERT_OR_FAIL( q.SizeApprox() == 0 );
            ASSERT_OR_FAIL( result.Load() );
        }

        {
            BlockingReaderWriterQueue<int> q( 100 );
            SimpleThread                   reader( [ & ]() {
                int item     = -1;
                int prevItem = -1;
                for ( int i = 0; i != 1000000; ++i ) {
                    if ( !q.DequeueWaitFor( item, 1000 ) ) {
                        --i;
                        continue;
                    }
                    if ( item <= prevItem ) {
                        result = 0;
                    }
                    prevItem = item;
                }
            } );
            SimpleThread                   writer( [ & ]() {
                for ( int i = 0; i != 1000000; ++i ) {
                    q.Enqueue( i );
                    for ( volatile int x = 0; x != 100; ++x )
                        ;
                }
            } );

            writer.join();
            reader.join();

            int item;
            ASSERT_OR_FAIL( q.SizeApprox() == 0 );
            ASSERT_OR_FAIL( !q.DequeueWaitFor( item, 0 ) );
            ASSERT_OR_FAIL( !q.DequeueWaitFor( item, 1 ) );
            ASSERT_OR_FAIL( result.Load() );
        }

        {
            BlockingReaderWriterQueue<UniquePtrWrapper> q( 100 );
            std::unique_ptr<int>                        p{ new int( 123 ) };
            q.Emplace( std::move( p ) );
            q.TryEmplace( std::move( p ) );
            UniquePtrWrapper item;
            ASSERT_OR_FAIL( q.DequeueWaitFor( item, 0 ) );
            ASSERT_OR_FAIL( item.get_value() == 123 );
            ASSERT_OR_FAIL( q.DequeueWaitFor( item, 0 ) );
            ASSERT_OR_FAIL( item.get_ptr() == nullptr );
            ASSERT_OR_FAIL( q.SizeApprox() == 0 );
        }

        return true;
    }

    bool Emplace() {
        ReaderWriterQueue<UniquePtrWrapper> q( 100 );
        std::unique_ptr<int>                p{ new int( 123 ) };
        q.Emplace( std::move( p ) );
        UniquePtrWrapper item;
        ASSERT_OR_FAIL( q.TryDequeue( item ) );
        ASSERT_OR_FAIL( item.get_value() == 123 );
        ASSERT_OR_FAIL( q.SizeApprox() == 0 );

        return true;
    }

    // This is what you have to do to TryEnqueue() a movable type, and demonstrates why TryEmplace() is useful
    bool TryEnqueue_fail_workaround() {
        ReaderWriterQueue<UniquePtrWrapper> q( 0 );
        {
            // A failed TryEnqueue() will still delete p
            std::unique_ptr<int> p{ new int( 123 ) };
            q.TryEnqueue( std::move( p ) );
            ASSERT_OR_FAIL( q.SizeApprox() == 0 );
            ASSERT_OR_FAIL( p == nullptr );
        }
        {
            // Workaround isn't pretty and potentially expensive - use TryEmplace() instead
            std::unique_ptr<int> p{ new int( 123 ) };
            UniquePtrWrapper     w( std::move( p ) );
            q.TryEnqueue( std::move( w ) );
            p = std::move( w.get_ptr() );
            ASSERT_OR_FAIL( q.SizeApprox() == 0 );
            ASSERT_OR_FAIL( p != nullptr );
            ASSERT_OR_FAIL( *p == 123 );
        }

        return true;
    }

    bool TryEmplace_fail() {
        ReaderWriterQueue<UniquePtrWrapper> q( 0 );
        std::unique_ptr<int>                p{ new int( 123 ) };
        q.TryEmplace( std::move( p ) );
        ASSERT_OR_FAIL( q.SizeApprox() == 0 );
        ASSERT_OR_FAIL( p != nullptr );
        ASSERT_OR_FAIL( *p == 123 );

        return true;
    }
};

void printTests( ReaderWriterQueueTests const& tests ) {
    std::printf( "   Supported tests are:\n" );

    std::vector<std::string> names;
    tests.getAllTestNames( names );
    for ( auto it = names.cbegin(); it != names.cend(); ++it ) {
        std::printf( "      %s\n", it->c_str() );
    }
}

// Basic test harness
int main( int argc, char** argv ) {
    bool                     disablePrompt = true;
    std::vector<std::string> selectedTests;

    // Disable buffering (so that when run in, e.g., Sublime Text, the output appears as it is written)
    std::setvbuf( stdout, nullptr, _IONBF, 0 );

    // Isolate the executable name
    std::string progName = argv[ 0 ];
    auto        slash    = progName.find_last_of( "/\\" );
    if ( slash != std::string::npos ) {
        progName = progName.substr( slash + 1 );
    }

    ReaderWriterQueueTests tests;

    // Parse command line options
    if ( argc == 1 ) {
        std::printf( "Running all unit tests for hakle::ReaderWriterQueue.\n(Run %s --help for other options.)\n\n", progName.c_str() );
    }
    else {
        bool printHelp    = false;
        bool printedTests = false;
        bool error        = false;
        for ( int i = 1; i < argc; ++i ) {
            if ( std::strcmp( argv[ i ], "--help" ) == 0 ) {
                printHelp = true;
            }
            else if ( std::strcmp( argv[ i ], "--disable-prompt" ) == 0 ) {
                disablePrompt = true;
            }
            else if ( std::strcmp( argv[ i ], "--run" ) == 0 ) {
                if ( i + 1 == argc || argv[ i + 1 ][ 0 ] == '-' ) {
                    std::printf( "Expected test name argument for --run option.\n" );
                    if ( !printedTests ) {
                        printTests( tests );
                        printedTests = true;
                    }
                    error = true;
                    continue;
                }

                if ( !tests.validateTestName( argv[ ++i ] ) ) {
                    std::printf( "Unrecognized test '%s'.\n", argv[ i ] );
                    if ( !printedTests ) {
                        printTests( tests );
                        printedTests = true;
                    }
                    error = true;
                    continue;
                }

                selectedTests.push_back( argv[ i ] );
            }
            else {
                std::printf( "Unrecognized option '%s'.\n", argv[ i ] );
                error = true;
            }
        }

        if ( error || printHelp ) {
            if ( error ) {
                std::printf( "\n" );
            }
            std::printf( "%s\n    Description: Runs unit tests for moodycamel::ReaderWriterQueue\n", progName.c_str() );
            std::printf( "    --help            Prints this help blurb\n" );
            std::printf( "    --run test        Runs only the specified test(s)\n" );
            std::printf( "    --disable-prompt  Disables prompt before exit when the tests finish\n" );
            return error ? -1 : 0;
        }
    }

    int exitCode = 0;

    bool result;
    if ( selectedTests.size() > 0 ) {
        result = tests.run( selectedTests );
    }
    else {
        result = tests.run();
    }

    if ( result ) {
        std::printf( "All %stests passed.\n", ( selectedTests.size() > 0 ? "selected " : "" ) );
    }
    else {
        std::printf( "Test(s) failed!\n" );
        exitCode = 2;
    }

    if ( !disablePrompt ) {
        std::printf( "Press ENTER to exit.\n" );
        getchar();
    }
    return exitCode;
}
