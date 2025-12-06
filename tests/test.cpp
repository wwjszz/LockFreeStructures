#include "../ReaderWriterQueue/readerwriterqueue.h"

#include "simplethread.h"

using namespace hakle;

#include <cstdlib>
#include <exception>
#include <fstream>
#include <cstdlib>		// rand()
//#include <unistd.h>		// usleep()

void unpredictableDelay( int extra = 0 ) {
    /*	if ((rand() & 4095) == 0) {
            usleep(2000 + extra);	// in microseconds
        }*/
}

int main( int argc, char** argv ) {
    // Disable buffering (so that when run in, e.g., Sublime Text, the output appears as it is written)
    std::setvbuf( stdout, nullptr, _IONBF, 0 );

    std::printf( "Running stability test for hakle::ReaderWriterQueue.\n" );
    std::printf( "Logging to 'log.txt'. Press CTRL+C to quit.\n\n" );

    std::ofstream log( "log.txt" );

    try {
        for (unsigned int i = 0; i < 5; ++i) {
            log << "Test #" << i << std::endl;
            std::printf( "Test #%d\n", i );

            BlockingReaderWriterQueue<unsigned long long> q( ( rand() % 32 ) + 1 );

            SimpleThread writer( [&]() {
                for (unsigned long long j = 0; j < 1024ULL * 1024ULL * 32ULL; ++j) {
                    unpredictableDelay( 500 );
                    q.Enqueue( j );
                }
            } );

            SimpleThread reader( [&]() {
                bool               canLog = true;
                unsigned long long element;
                for (unsigned long long j = 0; j < 1024ULL * 1024ULL * 32ULL;) {
                    if (canLog && ( j & ( 1024 * 1024 * 16 - 1 ) ) == 0) {
                        log << "  ... iteration " << j << std::endl;
                        std::printf( "  ... iteration %llu\n", j );
                        canLog = false;
                    }
                    unpredictableDelay();
                    if (q.TryDequeue( element )) {
                        if (element != j) {
                            log << "  ERROR DETECTED: Expected to read " << j << " but found " << element << std::endl;
                            std::printf( "  ERROR DETECTED: Expected to read %llu but found %llu", j, element );
                        }
                        ++j;
                        canLog = true;
                    }
                }
                if (q.TryDequeue( element )) {
                    log << "  ERROR DETECTED: Expected queue to be empty" << std::endl;
                    std::printf( "  ERROR DETECTED: Expected queue to be empty\n" );
                }
            } );

            writer.join();
            reader.join();
        }
    }
    catch (std::exception const& ex) {
        log << "  ERROR DETECTED: Exception thrown: " << ex.what() << std::endl;
        std::printf( "  ERROR DETECTED: Exception thrown: %s\n", ex.what() );
    }

    return 0;
}