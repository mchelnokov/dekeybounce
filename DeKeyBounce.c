/*
 * DeKeyBounce
 * A daemon that avoids key bounce on some keyboard types.
 *
 * Copyright (c) 2008 Michael Chelnokov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <CoreFoundation/CoreFoundation.h>
#include <ApplicationServices/ApplicationServices.h>

#include <sys/types.h>
#include <unistd.h>
#include <mach/mach.h>

#define DEFAULT_MIN_TIMESTAMP_DIFF 20UL /* 20 ms */

typedef struct _KeyData {

	uint64_t nKeyCode;
	uint64_t nLastKeyUpTimestamp;

} KeyData;

static CFMachPortRef theSignalPort = NULL;
static CFRunLoopSourceRef theSignalSource = NULL;
static mach_port_t theRawSignalPort = MACH_PORT_NULL;

static CFMutableSetRef theKeySet = NULL;
static CFMachPortRef theEventTap = NULL;
static CFRunLoopSourceRef theEventTapSource = NULL;
static CGEventTimestamp theMinTimestampDiff = 0;

static Boolean InitSignalHandling(void);
static void DeinitSignalHandling(void);
static void SignalHandler(int nSignal);
static void SignalCallBack(CFMachPortRef rPort, void *pMessage, CFIndex nSize, void *pInfo);

static Boolean Init(void);
static CGEventRef OnKeyEvent(CGEventTapProxy pProxy, CGEventType aEventType, CGEventRef rEvent, void *pInfo);
static void Deinit(void);

static const void *RetainKeyData(CFAllocatorRef rAllocator, const void *pValue);
static void ReleaseKeyData(CFAllocatorRef rAllocator, const void *pValue);
static Boolean IsKeyDataEqual(const void *pValue1, const void *pValue2);
static CFHashCode KeyDataHash(const void *pValue);

int main (int argc, const char * argv[]) {

	if(geteuid() != 0) // 0 is root
		return 1; // incorrect using
	if(getppid() != 1) // 1 is init
		return 1; // incorrect using
	if(!InitSignalHandling())
		return 1;
	if(argc > 1)
		theMinTimestampDiff = strtoul(argv[1], NULL, 10);
	if(theMinTimestampDiff == 0)
		theMinTimestampDiff = DEFAULT_MIN_TIMESTAMP_DIFF;
	theMinTimestampDiff *= 1000000; // from ms to ns
	if(!Init()) {
		DeinitSignalHandling();
		return 1;
	}
	CFRunLoopRun();
	Deinit();
	DeinitSignalHandling();
    return 0;

}

static Boolean InitSignalHandling(void) {

	Boolean isSuccess = FALSE;
	do { // just for break
		CFMachPortContext aSignalContext = { 0, NULL, NULL, NULL, NULL };
		theSignalPort = CFMachPortCreate(NULL, SignalCallBack, &aSignalContext, NULL);
		if(theSignalPort == NULL)
			break;
		theSignalSource = CFMachPortCreateRunLoopSource(NULL, theSignalPort, 0);
		if(theSignalSource == NULL)
			break;
		CFRunLoopAddSource(CFRunLoopGetCurrent(), theSignalSource, kCFRunLoopDefaultMode);
		theRawSignalPort = CFMachPortGetPort(theSignalPort);
		struct sigaction aSignalAction;
		bzero(&aSignalAction, sizeof aSignalAction);
		// handled signals
		aSignalAction.sa_handler = SignalHandler;
		if(sigaction(SIGHUP, &aSignalAction, NULL) != 0)
			break;
		if(sigaction(SIGINT, &aSignalAction, NULL) != 0)
			break;
		if(sigaction(SIGTERM, &aSignalAction, NULL) != 0)
			break;
		// ignored signals
		aSignalAction.sa_handler = SIG_IGN;
		if(sigaction(SIGPIPE, &aSignalAction, NULL) != 0)
			break;
		isSuccess = TRUE;
	} while(0);
	if(!isSuccess) {
		DeinitSignalHandling();
		return FALSE;
	}
	return TRUE;
	
}

static void DeinitSignalHandling(void) {

	struct sigaction aSignalAction;
	bzero(&aSignalAction, sizeof aSignalAction);
	aSignalAction.sa_handler = SIG_DFL;
	sigaction(SIGPIPE, &aSignalAction, NULL);
	sigaction(SIGTERM, &aSignalAction, NULL);
	sigaction(SIGINT, &aSignalAction, NULL);
	sigaction(SIGHUP, &aSignalAction, NULL);
	theRawSignalPort = MACH_PORT_NULL;
	if(theSignalSource != NULL) {
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), theSignalSource, kCFRunLoopDefaultMode);
		CFRelease(theSignalSource);
		theSignalSource = NULL;
	}
	if(theSignalPort != NULL) {
		CFRelease(theSignalPort);
		theSignalPort = NULL;
	}

}

static void SignalHandler(int nSignal) {

	if(theRawSignalPort == MACH_PORT_NULL)
		return; // ignore signal since our process is being terminated

	mach_msg_header_t aMachHeader;
	aMachHeader.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
	aMachHeader.msgh_remote_port = theRawSignalPort;
	aMachHeader.msgh_local_port = MACH_PORT_NULL;
	aMachHeader.msgh_size = sizeof aMachHeader;
	aMachHeader.msgh_id = nSignal;
	mach_msg_send(&aMachHeader); // if it will fail then that is the destiny

}

static void SignalCallBack(CFMachPortRef rPort, void *pMessage, CFIndex nSize, void *pInfo) {

	mach_msg_header_t *pMachHeader = (mach_msg_header_t *)pMessage;
	switch(pMachHeader->msgh_id) {
	case SIGHUP:
		break;
	case SIGINT:
	case SIGTERM:
		CFRunLoopStop(CFRunLoopGetCurrent());
		break;
	}

}

static Boolean Init(void) {

	Boolean isSuccess = FALSE;
	do { // just for break
		CFSetCallBacks aKeySetCallBacks = { 0, RetainKeyData, ReleaseKeyData, NULL, IsKeyDataEqual, KeyDataHash };
		theKeySet = CFSetCreateMutable(NULL, 0, &aKeySetCallBacks);
		if(!theKeySet)
			break;
		CGEventMask aEventMask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);
		theEventTap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, 0 /*kCGEventTapOptionDefault*/, aEventMask, OnKeyEvent, NULL);
		if(!theEventTap)
			break;
		theEventTapSource = CFMachPortCreateRunLoopSource(NULL, theEventTap, 0);
		if(!theEventTapSource)
			break;
		CFRunLoopAddSource(CFRunLoopGetCurrent(), theEventTapSource, kCFRunLoopDefaultMode);
		isSuccess = TRUE;
	} while(0);
	if(!isSuccess) {
		Deinit();
		return FALSE;
	}
	return TRUE;

}

static CGEventRef OnKeyEvent(CGEventTapProxy pProxy, CGEventType aEventType, CGEventRef rEvent, void *pInfo) {

	KeyData aNewKeyData;
	aNewKeyData.nKeyCode = CGEventGetIntegerValueField(rEvent, kCGKeyboardEventKeycode);
	aNewKeyData.nLastKeyUpTimestamp = CGEventGetTimestamp(rEvent);
	KeyData *pOldKeyData = (KeyData *)CFSetGetValue(theKeySet, &aNewKeyData);

	switch(aEventType) {

	case kCGEventKeyDown:
		if(!pOldKeyData)
			break;
		if(pOldKeyData->nLastKeyUpTimestamp == 0) {
			rEvent = NULL;
			break;
		}
		if(aNewKeyData.nLastKeyUpTimestamp < (pOldKeyData->nLastKeyUpTimestamp + theMinTimestampDiff)) {
			pOldKeyData->nLastKeyUpTimestamp = 0;
			rEvent = NULL;
			break;
		}
		break;

	case kCGEventKeyUp:
		if(!pOldKeyData) {
			CFSetAddValue(theKeySet, &aNewKeyData);
			break;
		}
		if(pOldKeyData->nLastKeyUpTimestamp == 0) {
			pOldKeyData->nLastKeyUpTimestamp = aNewKeyData.nLastKeyUpTimestamp;
			rEvent = NULL;
			break;
		}
		pOldKeyData->nLastKeyUpTimestamp = aNewKeyData.nLastKeyUpTimestamp;
		break;

	}
	return rEvent;

}

static void Deinit(void) {

	if(theEventTapSource) {
		CFRunLoopRemoveSource(CFRunLoopGetCurrent(), theEventTapSource, kCFRunLoopDefaultMode);
		CFRelease(theEventTapSource);
		theEventTapSource = NULL;
	}
	if(theEventTap) {
		CFRelease(theEventTap);
		theEventTap = NULL;
	}
	if(theKeySet) {
		CFRelease(theKeySet);
		theKeySet = NULL;
	}

}

static const void *RetainKeyData(CFAllocatorRef rAllocator, const void *pValue) {

	KeyData *pNewKeyData = CFAllocatorAllocate(rAllocator, sizeof(KeyData), 0);
	if(pNewKeyData) {
		pNewKeyData->nKeyCode = ((const KeyData *)pValue)->nKeyCode;
		pNewKeyData->nLastKeyUpTimestamp = ((const KeyData *)pValue)->nLastKeyUpTimestamp;
	}
	return pNewKeyData;

}

static void ReleaseKeyData(CFAllocatorRef rAllocator, const void *pValue) {

	CFAllocatorDeallocate(rAllocator, (void *)pValue);

}

static Boolean IsKeyDataEqual(const void *pValue1, const void *pValue2) {

	return (((const KeyData *)pValue1)->nKeyCode == ((const KeyData *)pValue2)->nKeyCode);

}

static CFHashCode KeyDataHash(const void *pValue) {

	return (CFHashCode)((const KeyData *)pValue)->nKeyCode;

}
