/*
* ObjectPool.cpp - This file is the part of the IISKeywordsFilter.
* Copyright (C) 2010 Dean Lee <deanlee3@gmail.com> <http://www.deanlee.cn/>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
* 
* Acknowledgements:
*   <> Many thanks to all of you, who have encouraged me to update my articles
*     and code, and who sent in bug reports and fixes.
* 
*/


#include "stdafx.h"
#include "objectpool.h"
template <typename T>
inline T MemAlign( T val, size_t alignment )
{
	return (T)( ( (size_t)val + alignment - 1 ) & ~( alignment - 1 ) );
}

#define _SEGMENT_HEADER_SIGNATURE 'Sgmt'
typedef struct _SEGMENT
{
#ifdef _DEBUG
	unsigned long Signiture;
#endif //_DEBUG	
	DLINK   _link;
	SLINK   FreeListHead;
	UINT    nNumberOfFreeBuffer;
}SEGMENT,*PSEGMENT;



#define _BUFFER_HEADER_SIGNATURE 'Bffr'
typedef struct _BUFFER_HEADER
{
#ifdef   _DEBUG
	unsigned long Signiture;
#endif //_DEBUG		
	unsigned int nBufferIndex;
}BUFFER_HEADER,*PBUFFER_HEADER;
/*///////////////////////////////////////////////////////////////////////////////////////
Create()

Creates the buffer pool object

IN const unsigned int   nBufferSize					Buffer Size
IN const unsigned int	nNumberOfBuffersInSegment   Number of Buffers in Segment
IN const unsigned int	nNumberOfSegmentsStart      Number of Segments to start with
IN const unsigned int	nNumberOfSegmentsLowMark    Minimum Number of Segments 
IN const unsigned int	nNumberOfSegmentsHighMark   Maximum Number of Segments  (Default: -1 No Limit to segment number)
IN const double			lfRatioForSegmentDeletion   Percent of Accumulated free Buffers in all segment, Of a segment
In order to free a Not used segment (Default: 0.333 if 33% of a segment is free and there is an empty segment available Then free it) 
///////////////////////////////////////////////////////////////////////////////////////*/

CObjectPool::CObjectPool(IN const size_t   nBufferSize,
						 IN const unsigned int	nNumberOfBuffersInSegment,
						 IN const unsigned int	nNumberOfSegmentsStart,
						 IN const unsigned int	nNumberOfSegmentsLowMark,
						 IN const unsigned int	nNumberOfSegmentsHighMark /*= -1    */,
						 IN const double			lfRatioForSegmentDeletion /*= 0.333 */)
{
	SYSTEM_INFO SysInfo;
	GetSystemInfo(&SysInfo);
	m_dwPageSize = SysInfo.dwPageSize;
	InitializeCriticalSection(&m_CriticalSection);

	// check parameters
	ATLASSERT(	nNumberOfBuffersInSegment != 0 &&
		nNumberOfSegmentsStart != 0 &&
		nNumberOfSegmentsStart >= nNumberOfSegmentsLowMark &&
		nNumberOfSegmentsStart <= nNumberOfSegmentsHighMark	&&
		nNumberOfSegmentsLowMark <= nNumberOfSegmentsHighMark	);

	// init segment list
	DLINK_Initialize(&m_SegmentsListHead);

	// init all member variables
	m_nFreeBuffers                   = 0;
	m_nNumberOfBuffersInSegment      = nNumberOfBuffersInSegment;
	m_nNumberOfSegmentsLowMark       = nNumberOfSegmentsLowMark;
	m_nNumberOfSegmentsHighMark      = nNumberOfSegmentsHighMark;
	m_nBufferSize                    = max(nBufferSize, sizeof(SLINK));// at least link size
	m_nNumberOfSegments              = 0;
	m_pSegmentCandidateForDeletion   = NULL;
	m_nCandidateDeletionLimit        = (unsigned int) (nNumberOfBuffersInSegment * lfRatioForSegmentDeletion);
	// allocate needed segments
	for (unsigned int i = 0; i < nNumberOfSegmentsStart; ++i)
	{
		if (!AllocateSegment())
		{			
			ATLASSERT(false);
			Destroy();
			throw ERROR_OUTOFMEMORY;
		}
	}
}


CObjectPool::~CObjectPool()
{
	Destroy();
}

void* CObjectPool::Allocate()
{	
	PSEGMENT       pSegment;
	void*          pBuffer = NULL;
	//lock
	EnterCriticalSection(&m_CriticalSection);

	if (!m_nFreeBuffers)
	{
		// no buffers left

		if (m_nNumberOfSegments > m_nNumberOfSegmentsHighMark)
		{
			// riched the higher mark
			// no more allocations
			goto exit_func;
		}
		if (m_pSegmentCandidateForDeletion  != NULL)
		{
			// use the candidate for deletion
			DLINK_InsertNext(&m_SegmentsListHead, &m_pSegmentCandidateForDeletion->_link);
			m_pSegmentCandidateForDeletion = NULL;
			// correct counts
			m_nFreeBuffers += m_nNumberOfBuffersInSegment;
			++m_nNumberOfSegments; 
		}
		else if (!AllocateSegment())
		{		
			ATLASSERT(false);
			goto exit_func;
		}
	}

	// at least one buffer free if here
	ATLASSERT(m_nFreeBuffers != 0);

	// there is at least one buffer free in the first segment
	pSegment = CONTAINING_RECORD(m_SegmentsListHead._next, SEGMENT, _link);

	ATLASSERT(pSegment->Signiture == _SEGMENT_HEADER_SIGNATURE);

	// pop the buffer
	pBuffer = SLINK_Pop(&pSegment->FreeListHead);     

	if (!--pSegment->nNumberOfFreeBuffer)
	{
		// the segment is all taken, send it to the end of the list.
		ATLASSERT(SLINK_IsEmpty(&pSegment->FreeListHead));
		ATLASSERT(!DLINK_IsEmpty(&m_SegmentsListHead));
		DLINK_Remove(&pSegment->_link);
		DLINK_InsertPrev(&m_SegmentsListHead, &pSegment->_link);
	}

	// one less free
	--m_nFreeBuffers;

exit_func:

	LeaveCriticalSection(&m_CriticalSection);

#ifdef _DEBUG
	memset(pBuffer, 'E', m_nBufferSize);
#endif
	return pBuffer;
}

/*
Free()

Free one buffer from the buffer pool 
This function is thread safe

*/

void  CObjectPool::Free(const void* pBuffer)
{
	PSEGMENT   pSegment;
	unsigned char* p = (unsigned char*)pBuffer;

#ifdef _DEBUG
	memset(p, 'E', m_nBufferSize );
#endif

	EnterCriticalSection(&m_CriticalSection);

	// go back to the header
	p -= sizeof(BUFFER_HEADER);

	ATLASSERT(((PBUFFER_HEADER)p)->Signiture == _BUFFER_HEADER_SIGNATURE);


	// get the segment, by calculating the gap between the buffer and the start of the segment
	pSegment = (PSEGMENT)(p - (((PBUFFER_HEADER)p)->nBufferIndex * (sizeof(BUFFER_HEADER) + m_nBufferSize))) - 1;

	//check validity of segment
	ATLASSERT(pSegment->Signiture == _SEGMENT_HEADER_SIGNATURE);

	// push freeed buffer to segment free buffers list
	SLINK_Push(&pSegment->FreeListHead, (PSLINK)pBuffer);

	// one more free buffer in segment
	++pSegment->nNumberOfFreeBuffer;

	//if this segment is not in front of the segments list insert it to front
	if(m_SegmentsListHead._next != &pSegment->_link)
	{		
		DLINK_Remove(&pSegment->_link);
		DLINK_InsertNext(&m_SegmentsListHead, &pSegment->_link);
	}

	// check if we can delete the candidate for deletion segment 
	if  (m_pSegmentCandidateForDeletion != NULL &&
		m_nFreeBuffers > m_nCandidateDeletionLimit)
	{

#ifdef _DEBUG
		memset(m_pSegmentCandidateForDeletion, 'E', sizeof(SEGMENT) + 
			m_nNumberOfBuffersInSegment * (sizeof(BUFFER_HEADER) + m_nBufferSize));
#endif
		VirtualFree(m_pSegmentCandidateForDeletion, 0, MEM_RELEASE);
		m_pSegmentCandidateForDeletion = NULL;
	}

	if (pSegment->nNumberOfFreeBuffer == m_nNumberOfBuffersInSegment &&
		m_nNumberOfSegments            > m_nNumberOfSegmentsLowMark)
	{
		// if all the segment is freeed and we are more then the low mark
		if (m_pSegmentCandidateForDeletion  != NULL)
		{   // there is a candidate segment for deletion
			// remove the segment
			FreeSegment(pSegment);
		}
		else
		{   // no candidate segment for deletion
			// use this segment as a candidate segment for deletion
			m_pSegmentCandidateForDeletion = pSegment;
			DLINK_Remove(&m_pSegmentCandidateForDeletion->_link);
			DLINK_Initialize(&m_pSegmentCandidateForDeletion->_link);
			m_nFreeBuffers -= m_nNumberOfBuffersInSegment;
			--m_nNumberOfSegments; 
		}
	}

	++m_nFreeBuffers;

	LeaveCriticalSection(&m_CriticalSection);
}

/*
Destroy()

Destroy the buffer pool object

*/

void CObjectPool::Destroy()
{	
	PDLINK   d_link;
	PSEGMENT pSegment;

	EnterCriticalSection(&m_CriticalSection);

	ATLASSERT(m_nFreeBuffers == (m_nNumberOfSegments * m_nNumberOfBuffersInSegment));

	d_link = m_SegmentsListHead._next;

	// free all segments
	while (d_link != &m_SegmentsListHead)
	{
		pSegment = CONTAINING_RECORD(d_link, SEGMENT, _link);
		ATLASSERT(pSegment->Signiture  == _SEGMENT_HEADER_SIGNATURE);
		ATLASSERT(pSegment->nNumberOfFreeBuffer == m_nNumberOfBuffersInSegment);
		d_link =  d_link->_next;
		FreeSegment(pSegment);
	}

	// free candidate for deletion if there is one
	if (m_pSegmentCandidateForDeletion != NULL)
	{
		VirtualFree(m_pSegmentCandidateForDeletion, 0, MEM_RELEASE);
		//		delete m_pSegmentCandidateForDeletion;
	}


	LeaveCriticalSection(&m_CriticalSection);

	DeleteCriticalSection(&m_CriticalSection);		
}

/*
AllocateSegment()

Allocates a Segment
Used only within the class

*/

bool CObjectPool::AllocateSegment()
{
	PSEGMENT         pSegment;

	// calculate segment size
	// unsigned int segment_size = sizeof(SEGMENT) +  m_nBufferSize;

	unsigned int segment_size = MemAlign(sizeof(SEGMENT) + m_nNumberOfBuffersInSegment  * 
		(sizeof(BUFFER_HEADER) + m_nBufferSize), m_dwPageSize);

	// allocate segment
	pSegment = (PSEGMENT)VirtualAlloc(NULL, segment_size, 
		MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
	if (pSegment == NULL)
	{      
		ATLASSERT(false);
		return false;
	}

#ifdef _DEBUG
	memset(pSegment, 'E', segment_size);
	pSegment->Signiture  = _SEGMENT_HEADER_SIGNATURE;
#endif

	// init free list head
	SLINK_Initialize(&pSegment->FreeListHead);

	//all buffers in segment are free
	pSegment->nNumberOfFreeBuffer = m_nNumberOfBuffersInSegment;

	unsigned char*  p = (unsigned char*)(pSegment + 1);

	for (UINT i = 0; i < m_nNumberOfBuffersInSegment; ++i)
	{
		// init buffer header      
#ifdef _DEBUG
		((PBUFFER_HEADER)p)->Signiture = _BUFFER_HEADER_SIGNATURE;
#endif

		//initialize buffer index
		((PBUFFER_HEADER)p)->nBufferIndex = i;

		//use the buffer itself for the linked list of free buffer in the segment
		p   += sizeof(BUFFER_HEADER);

		//add buffer to free buffers list       
		SLINK_Push(&pSegment->FreeListHead , (PSLINK)p);

		//goto the next buffer
		p += m_nBufferSize;
	}

	// insert segment to segments list
	DLINK_InsertNext(&m_SegmentsListHead, &pSegment->_link);

	// update buffer pool variables
	m_nFreeBuffers += m_nNumberOfBuffersInSegment;
	++m_nNumberOfSegments;
	return true;
}

/*
FreeSegment()

Free a Segment
Used only within the class

*/

void CObjectPool::FreeSegment(IN PSEGMENT pSegment)
{

	ATLASSERT(pSegment != NULL && 
		pSegment->nNumberOfFreeBuffer ==	m_nNumberOfBuffersInSegment	&&
		pSegment->Signiture ==  _SEGMENT_HEADER_SIGNATURE);

	m_nFreeBuffers -= m_nNumberOfBuffersInSegment;   
	--m_nNumberOfSegments;

	// remove from list
	DLINK_Remove(&pSegment->_link);

#ifdef _DEBUG
	memset(pSegment, 'E', sizeof(SEGMENT) + 
		m_nNumberOfBuffersInSegment * (sizeof(BUFFER_HEADER) + m_nBufferSize));
#endif

	// delete
	VirtualFree(pSegment, 0, MEM_RELEASE);
	//delete pSegment;
}