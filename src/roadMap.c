/*
Copyright 2007, 2008 Daniel Zerbino (zerbino@ebi.ac.uk)

    This file is part of Velvet.

    Velvet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Velvet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Velvet; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

*/
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "globals.h"
#include "graph.h"
#include "recycleBin.h"
#include "tightString.h"
#include "roadMap.h"
#include "utility.h"
#include "kmer.h"

#ifndef NULL
#define NULL 0
#endif

union positionPtr {
	ShortLength coord;
	IDnum nodeID;
}  ATTRIBUTE_PACKED;

struct annotation_st {
	ShortLength position;	// 32
	union positionPtr start;	// 32
	union positionPtr finish;	// 32
	ShortLength length;	// 32
	IDnum sequenceID;	// 32
}  ATTRIBUTE_PACKED;

struct roadmap_st {
	ShortLength annotationCount;
}  ATTRIBUTE_PACKED;

// Creates empty RoadMap
RoadMap *newRoadMap()
{
	return callocOrExit(1, RoadMap);
}

IDnum getAnnotationCount(RoadMap * rdmap)
{
	return rdmap->annotationCount;
}

Coordinate getFinish(Annotation * annot)
{
	return annot->finish.coord;
}

IDnum getAnnotSequenceID(Annotation * annot, RoadMapArray * rdmaps)
{
	if (rdmaps && rdmaps->indexOrder)
		return rdmaps->indexOrder[annot->sequenceID];
	else
		return annot->sequenceID;
}

Coordinate getStart(Annotation * annot)
{
	return annot->start.coord;
}

Coordinate getPosition(Annotation * annot)
{
	return annot->position;
}

Coordinate getAnnotationLength(Annotation * annot)
{
	if (annot == NULL)
		return 0;

	return annot->length;
}

//////////////////////////////////////////////////////////////
// Index conversion table
//////////////////////////////////////////////////////////////

typedef struct indexConversion_st {
	IDnum initialIndex;
	IDnum actualIndex;
} IndexConversion;

#ifdef OPENMP
static int compareIndexConversions(const void * A, const void * B) {
	IndexConversion * rdmapA = (IndexConversion *) A; 
	IndexConversion * rdmapB = (IndexConversion *) B; 
	IDnum diff = rdmapA->actualIndex - rdmapB->actualIndex;

	if (diff > 0) 
		return 1;
	else if (diff == 0)
		return 0;
	else 
		return -1;
}
#endif

//////////////////////////////////////////////////////////////

// Imports roadmap from the appropriate file format
// Memory allocated within the function
RoadMapArray *importRoadMapArray(char *filename)
{
	FILE *file;
	const int maxline = 100;
	char *line = mallocOrExit(maxline, char);
	RoadMap *rdmap = NULL;
	IDnum seqID;
	Coordinate position, start, finish;
	Annotation *nextAnnotation;
	RoadMapArray *result = mallocOrExit(1, RoadMapArray);
	IDnum sequenceCount;
	IDnum annotationCount = 0;
	short short_var;
	long long_var, long_var2;
	long long longlong_var, longlong_var2, longlong_var3;
#ifdef OPENMP
	IndexConversion * indexConversionTable, * currentIndexConversionEntry;
	IDnum counter = 1;
#endif

	velvetLog("Reading roadmap file %s\n", filename);

	file = fopen(filename, "r");
	if (!fgets(line, maxline, file))
		exitErrorf(EXIT_FAILURE, true, "%s incomplete.", filename);
	sscanf(line, "%ld\t%ld\t%i\t%hi\n", &long_var, &long_var2, &(result->WORDLENGTH), &short_var);
	sequenceCount = (IDnum) long_var;
	resetWordFilter(result->WORDLENGTH);
	result->length = sequenceCount;
	result->referenceCount = long_var2;
	result->array = callocOrExit(sequenceCount, RoadMap);
	result->double_strand = (boolean) short_var;

	while (fgets(line, maxline, file) != NULL)
		if (line[0] != 'R')
			annotationCount++;

	result->annotations = callocOrExit(annotationCount, Annotation);
	nextAnnotation = result->annotations;
	fclose(file);

	file = fopen(filename, "r");

#ifdef OPENMP
	indexConversionTable = callocOrExit(sequenceCount, IndexConversion);
	currentIndexConversionEntry = indexConversionTable;
#endif

	rdmap = result->array - 1;
	if (!fgets(line, maxline, file))
		exitErrorf(EXIT_FAILURE, true, "%s incomplete.", filename);
	while (fgets(line, maxline, file) != NULL) {
		if (line[0] == 'R') {
#ifdef OPENMP
		    	sscanf(line, "%*s %ld\n", &long_var);
			currentIndexConversionEntry->initialIndex = counter++;
			currentIndexConversionEntry->actualIndex = (IDnum) long_var; 
			currentIndexConversionEntry++;
#endif
			rdmap++;
		} else {
			sscanf(line, "%ld\t%lld\t%lld\t%lld\n", &long_var,
			       &longlong_var, &longlong_var2, &longlong_var3);
			seqID = (IDnum) long_var;
			position = (Coordinate) longlong_var;
			start = (Coordinate) longlong_var2;
			finish = (Coordinate) longlong_var3;
			nextAnnotation->sequenceID = seqID;
			nextAnnotation->position = position;
			nextAnnotation->start.coord = start;
			nextAnnotation->finish.coord = finish;

			if (seqID > 0)
				nextAnnotation->length = finish - start;
			else
				nextAnnotation->length = start - finish;

			rdmap->annotationCount++;
			nextAnnotation++;
		}
	}
	velvetLog("%d roadmaps read\n", sequenceCount);

#ifdef OPENMP
	// Sort the index conversion table 
	qsort(indexConversionTable, sequenceCount, sizeof(IndexConversion), compareIndexConversions);

	// Record the order of the sequence indices
	result->indexOrder = callocOrExit(sequenceCount, IDnum);
	for (counter = 0; counter < sequenceCount; counter++)
		result->indexOrder[counter] = indexConversionTable[counter].initialIndex;
	free(indexConversionTable);
#endif

	fclose(file);
	free(line);
	return result;
}

// Imports annotations from the appropriate file format
// Memory allocated within the function
Annotation *importAnnotations(FILE *file, IDnum * readIndex, IDnum * annotationCount)
{
	char line[MAXLINE];
	long long_var;
	long long longlong_var, longlong_var2, longlong_var3;
	IDnum arraySize = 8; 
	Annotation * annotations = callocOrExit(arraySize, Annotation);
	Annotation * nextAnnotation = annotations;

	// Read data and fill array
	nextAnnotation = annotations;
	sscanf(line, "%*s %ld\n", &long_var);
	*readIndex = (IDnum) long_var - 1;
	while(fgets(line, MAXLINE, file) && line[0] != 'R') {
		sscanf(line, "%ld\t%lld\t%lld\t%lld\n", &long_var,
		       &longlong_var, &longlong_var2, &longlong_var3);
		nextAnnotation->sequenceID = (IDnum) long_var;
		nextAnnotation->position = (Coordinate) longlong_var;
		nextAnnotation->start.coord = (Coordinate) longlong_var2;
		nextAnnotation->finish.coord = (Coordinate) longlong_var3;

		if (nextAnnotation->sequenceID > 0)
			nextAnnotation->length = nextAnnotation->finish.coord - nextAnnotation->start.coord;
		else
			nextAnnotation->length = nextAnnotation->start.coord - nextAnnotation->finish.coord;

		nextAnnotation++;	
		(*annotationCount)++;

		// Realloc memory if necessary
		if (*annotationCount == arraySize) {
			arraySize *= 2;
			annotations = reallocOrExit(annotations, arraySize, Annotation);
		}
	}

	annotations = reallocOrExit(annotations, *annotationCount, Annotation);
	return annotations;
}

RoadMap *getRoadMapInArray(RoadMapArray * array, IDnum index)
{
	return &(array->array[index]);
}

void setStartID(Annotation * annot, IDnum nodeID)
{
	annot->start.nodeID = nodeID;
}

void setFinishID(Annotation * annot, IDnum nodeID)
{
	annot->finish.nodeID = nodeID;
}

IDnum getStartID(Annotation * annot)
{
	return annot->start.nodeID;
}

IDnum getFinishID(Annotation * annot)
{
	return annot->finish.nodeID;
}

void incrementAnnotationCoordinates(Annotation * annot)
{
	annot->start.coord++;
	annot->finish.coord++;
}

Annotation *getNextAnnotation(Annotation * annot)
{
	return annot + 1;
}

void destroyRoadMapArray(RoadMapArray * rdmaps) {
	free(rdmaps->array);
	free(rdmaps->annotations);
	free(rdmaps);
} 
