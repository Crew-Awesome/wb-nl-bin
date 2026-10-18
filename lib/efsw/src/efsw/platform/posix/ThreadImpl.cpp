#include <efsw/Thread.hpp>
#include <efsw/platform/posix/ThreadImpl.hpp>

#if defined( EFSW_PLATFORM_POSIX )

#include <cassert>
#include <efsw/Debug.hpp>
#include <iostream>
#if defined(__linux__)
#include <sys/prctl.h>
#include <pthread.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__FreeBSD__)
#include <pthread.h>
#include <pthread_np.h>
#endif

namespace efsw { namespace Platform {

ThreadImpl::ThreadImpl( Thread* owner ) : mIsActive( false ) {
	mIsActive = pthread_create( &mThread, NULL, &ThreadImpl::entryPoint, owner ) == 0;

	if ( !mIsActive ) {
		efDEBUG( "Failed to create thread\n" );
	}
}

void ThreadImpl::wait() {
	// Wait for the thread to finish, no timeout
	if ( mIsActive ) {
		assert( pthread_equal( pthread_self(), mThread ) == 0 );

		pthread_join( mThread, NULL );

		mIsActive = false; // Reset the thread state
	}
}

void ThreadImpl::terminate() {
	if ( mIsActive ) {
#if !defined( __ANDROID__ ) && !defined( ANDROID )
		pthread_cancel( mThread );
#else
		pthread_kill( mThread, SIGUSR1 );
#endif

		mIsActive = false;
	}
}

void* ThreadImpl::entryPoint( void* userData ) {
	// The Thread instance is stored in the user data
	Thread* owner = static_cast<Thread*>( userData );

#if defined(__linux__)
	prctl( PR_SET_NAME, "weekbox-watch", 0, 0, 0 );
	pthread_setname_np( pthread_self(), "weekbox-watch" );
#elif defined(__APPLE__)
	pthread_setname_np( "weekbox-watch" );
#elif defined(__FreeBSD__)
	pthread_set_name_np( pthread_self(), "weekbox-watch" );
#endif

// Tell the thread to handle cancel requests immediatly
#ifdef PTHREAD_CANCEL_ASYNCHRONOUS
	pthread_setcanceltype( PTHREAD_CANCEL_ASYNCHRONOUS, NULL );
#endif

	// Forward to the owner
	owner->run();

	return NULL;
}

}} // namespace efsw::Platform

#endif
