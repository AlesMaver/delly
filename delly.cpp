/*
============================================================================
DELLY: Structural variant discovery by integrated PE mapping and SR analysis
============================================================================
Copyright (C) 2012 Tobias Rausch

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
============================================================================
Contact: Tobias Rausch (rausch@embl.de)
============================================================================
*/

#include <iostream>
#include <fstream>
#include <iomanip>
#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <boost/graph/connected_components.hpp>
#include <boost/program_options/cmdline.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/math/special_functions/pow.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <boost/functional/hash.hpp>
#include <boost/archive/tmpdir.hpp>
#include <boost/filesystem.hpp>
#include <boost/progress.hpp>

#include "api/BamIndex.h"

#include "memory_mapped_file.h"
#include "bam_file_adaptor.h"
#include "version.h"
#include "util.h"
#include "sam.h"
#include "fasta_reader.h"
#include "dna_score.h"
#include "align_config.h"
#include "align_gotoh.h"
#include "align_nw.h"
#include "align_nw_mat.h"
#include "extract.h"
#include "index.h"


#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
#include <stdio.h>
#include "kseq.h"
KSEQ_INIT(gzFile, gzread)

using namespace torali;

// Tags
struct DeletionTag;
struct DuplicationTag;
template<typename SvTag>
struct SVType {
};




// Config arguments
struct Config {
  bool peMode;
  unsigned short minMapQual;
  unsigned short madCutoff;
  unsigned int minimumFlankSize;
  unsigned int minimumSplitRead;
  unsigned int flankQuality;
  unsigned int graphPruning;
  float epsilon;
  float percentAbnormal;
  std::string svType;
  boost::filesystem::path outfile;
  boost::filesystem::path genome;
  boost::filesystem::path variants;
  boost::filesystem::path exclude;
  std::vector<boost::filesystem::path> files;
};

// Structural variant record
struct StructuralVariantRecord {
  int svStartBeg;
  int svStartEnd;
  int svEndBeg;
  int svEndEnd;
  int svStart;
  int svEnd;
  int peSupport;
  int srSupport;
  int wiggle;
  double srAlignQuality;
  unsigned int id;
  bool precise;
  uint16_t peMapQuality;
  std::string chr;
  std::string consensus;
};


// Unique paired-end data structure for single chromosome only
struct Hit {
  int32_t minPos;
  int32_t maxPos;
  
  Hit(BamTools::BamAlignment const& al) : minPos(std::min(al.Position, al.MatePosition)), maxPos(std::max(al.Position, al.MatePosition)) {}

  bool operator <(Hit const& other) const {
    return ((minPos<other.minPos) || ((minPos==other.minPos) && (maxPos<other.maxPos)));
  }
};

// Reduced bam alignment record data structure
struct BamAlignRecord {
  int32_t     Length;
  int32_t     RefID;         
  int32_t     Position;      
  int32_t     MateRefID; 
  int32_t     MatePosition;
  uint16_t    MapQuality;    
  int32_t     Median;
  int32_t     Mad;
  int32_t     maxNormalISize;
  uint32_t    AlignmentFlag; 

  BamAlignRecord(BamTools::BamAlignment const& al, uint16_t pairQuality, int32_t median, int32_t mad, int32_t maxISize) : Length(al.Length), RefID(al.RefID), Position(al.Position), MateRefID(al.MateRefID), MatePosition(al.MatePosition), MapQuality(pairQuality), Median(median), Mad(mad), maxNormalISize(maxISize), AlignmentFlag(al.AlignmentFlag) {}

};

// Sort reduced bam alignment records
template<typename TRecord>
struct SortBamRecords : public std::binary_function<TRecord, TRecord, bool>
{
  inline bool operator()(TRecord const& s1, TRecord const& s2) const {
    return ((s1.RefID<s2.RefID) || ((s1.RefID==s2.RefID) && (std::min(s1.Position,s1.MatePosition) < std::min(s2.Position,s2.MatePosition))));
  }
};

// SplitRead struct
struct SplitReadCoord {
  bool forwardRead;
  unsigned int offset;
  int lastKmer;
  int diag;
  std::vector<char> read;
};


template<typename TSequence>
struct OutputRead {
  bool forwardRead;
  unsigned int readOffset;
  TSequence seq;
};

template<typename TRecord>
struct SortSplitReadRecords : public std::binary_function<TRecord, TRecord, bool>
{
  inline bool operator()(TRecord const& s1, TRecord const& s2) {
    if (s1.offset == s2.offset) {
      if (s1.lastKmer == s2.lastKmer) return (s1.diag < s2.diag);
      else return (s1.lastKmer<s2.lastKmer);
    } else return (s1.offset < s2.offset);
  }
};


// Deletions
template<typename TUSize, typename TSize>
inline bool
_betterSplit(TUSize, TUSize, TUSize, TSize, TUSize oldOffset, TUSize skippedLength, TUSize initialLength, double epsilon, TUSize support, TUSize bestSupport, SVType<DeletionTag>) {
  return ((std::abs((((double) oldOffset + skippedLength) / (double) initialLength) - 1.0) <= epsilon) && (support > bestSupport));
}


// Duplications
template<typename TUSize, typename TSize>
inline bool
_betterSplit(TUSize refSize, TUSize refSizeRight, TUSize, TSize oldDiag, TUSize oldOffset, TUSize skippedLength, TUSize initialLength, double epsilon, TUSize support, TUSize bestSupport, SVType<DuplicationTag>) {
  return ((oldDiag < (TSize) refSizeRight) && ((oldDiag + (TSize) oldOffset) > (TSize) refSizeRight) &&  (std::abs((( (double) refSize - (double) oldOffset + skippedLength) / (double) initialLength) - 1.0) <= epsilon) && (support > bestSupport));
}


// Deletions
template<typename TUSize, typename TSize, typename TBamRecord>
inline bool
_translateSVCoordinates(TUSize, TUSize, TUSize, TSize diagBeg, TSize diagEnd, TUSize leftRefWalky, TUSize rightRefWalky, TUSize consLen, TBamRecord const& sv, TUSize& finalGapStart, TUSize& finalGapEnd, TUSize& predictedLength, SVType<DeletionTag>) {
  finalGapStart = diagBeg + leftRefWalky + 1;
  finalGapEnd = diagEnd + consLen - (consLen - rightRefWalky);
  predictedLength = finalGapEnd -  finalGapStart;
  TUSize annealed = (sv.svStartEnd - sv.svStartBeg);
  if ((finalGapStart>=annealed) || (finalGapEnd < annealed)) return true;
  else {
    predictedLength+=(sv.svEndBeg - sv.svStartEnd);
    finalGapStart+=sv.svStartBeg;
    finalGapEnd+=(sv.svEndBeg - annealed);
  } 
  return false;
}

// Duplications
template<typename TUSize, typename TSize, typename TBamRecord>
inline bool
_translateSVCoordinates(TUSize refSize, TUSize refSizeRight, TUSize refSizeLeft, TSize diagBeg, TSize diagEnd, TUSize leftRefWalky, TUSize rightRefWalky, TUSize consLen, TBamRecord const& sv, TUSize& finalGapStart, TUSize& finalGapEnd, TUSize& predictedLength, SVType<DuplicationTag>) {
  finalGapStart = diagBeg + leftRefWalky + 1;
  finalGapEnd = diagEnd + consLen - (consLen - rightRefWalky) + 1;
  if ((finalGapEnd <= refSizeRight) || (finalGapStart >= refSizeRight)) return true;
  unsigned int finalGapStartTmp = finalGapStart;
  finalGapStart = (refSizeLeft - (refSize - finalGapEnd)) + sv.svStartBeg - 1;
  finalGapEnd = refSizeLeft + finalGapStartTmp + sv.svStartBeg + (sv.svEndBeg - sv.svStartEnd);
  predictedLength = finalGapEnd - finalGapStart;
  return false;
}


template<typename TConfig, typename TStructuralVariantRecord, typename TTag>
inline
void searchSplit(TConfig const& c, TStructuralVariantRecord& sv, std::string const& svRefStr, std::set<std::string> const& splitReadSet, SVType<TTag> svType) {
  // Index SV reference
  typedef Index<int, int,  char, 11, 4> TIndex;
  TIndex index;
  index.indexSequence(svRefStr);

  // Define kmerHitCutoff
  unsigned int kmerHitCutoff = 2;
  if (TIndex::kmer_size < c.minimumFlankSize) kmerHitCutoff = c.minimumFlankSize - TIndex::kmer_size + 1;

  // Define supposed length of split
  unsigned int initialLength = (sv.svEnd - sv.svStart + 1);
  unsigned int skippedLength = (sv.svEndBeg -sv.svStartEnd);
  unsigned int refSize = index.refSequence.size();
  unsigned int refSizeLeft = sv.svStartEnd - sv.svStartBeg;
  unsigned int refSizeRight = refSize - refSizeLeft;

  // Collect all potential split sites
  typedef std::vector<SplitReadCoord> TReadVec;
  TReadVec readVec;

  // Iterate over all reads
  typedef std::pair<int, unsigned int> TKmerDiagPos;
  typedef std::vector<TKmerDiagPos> TDiag;
  TDiag forward;
  TDiag reverse;
  TDiag forwardReverse;

  typedef std::vector<char> TSequence;
  typedef std::set<std::string> TReadSet;
  TReadSet::const_iterator splitIter = splitReadSet.begin();
  for(;splitIter!=splitReadSet.end();++splitIter) {
    TSequence read;
    std::string::const_iterator readIt=splitIter->begin();
    for(;readIt!=splitIter->end();++readIt) read.push_back(dna5_encode[int(*readIt)]);
    unsigned int readLen = read.size();

    // Count kmer's 
    forward.clear();
    reverse.clear();
    index.diagonalForwardKmerCount(read, forward);
    index.diagonalReverseKmerCount(read, reverse);
    
    // No hits at all?
    bool alignDir = true;
    if (forward.empty() && reverse.empty()) continue;
    else {
      // Take better alignment
      if (forward.size()>reverse.size()) forwardReverse=forward;
      else {
	forwardReverse=reverse;
	alignDir=false;
      }
    }

    // Get the best split diagonals
    TDiag bestForwardReverse;
    sort(forwardReverse.begin(), forwardReverse.end());
    bool validForwardReverse = _findBestSupportDiagonal(c, forwardReverse, bestForwardReverse, kmerHitCutoff, readLen - TIndex::kmer_size + 1);

    // Get potential split-reads
    if (validForwardReverse) {
      std::sort(bestForwardReverse.begin(), bestForwardReverse.end()); // Sort diagonals
      typename TDiag::const_iterator itBest = bestForwardReverse.begin();
      typename TDiag::const_iterator itBestEnd = bestForwardReverse.end();
      for(;(itBest + 1)!=itBestEnd; ++itBest) {
	typename TDiag::const_iterator itBestNext = itBest + 1;
	for(;itBestNext !=itBestEnd; ++itBestNext) {
	  if (itBestNext->first - itBest->first >= 5) {   //Previously 20 for dups
	    SplitReadCoord sC;
	    sC.offset = (itBestNext->first - itBest->first);
	    sC.lastKmer = itBest->first + itBest->second;
	    sC.diag = itBest->first;
	    sC.read = read;
	    sC.forwardRead = alignDir;
	    readVec.push_back(sC);
	    //std::cout << seqIt->chrName << ',' << sC.offset << ',' << sC.lastKmer << ',' << sC.diag << ',' << sC.forwardRead << std::endl;
	  }
	}
      }
    } 
  }

  // All potential split-reads collected, try to get the split
  if (!readVec.empty()) {
    // Sort by offset
    std::sort(readVec.begin(), readVec.end(),  SortSplitReadRecords<SplitReadCoord>());
    
    // Find best supported offset
    typename TReadVec::const_iterator readVecIt = readVec.begin();
    typename TReadVec::const_iterator readVecItEnd = readVec.end();
    int oldDiag = readVecIt->diag;
    int oldKmer = readVecIt->lastKmer;
    unsigned int oldOffset = readVecIt->offset;
    unsigned int support = 1;
    unsigned int bestSupport = 0;
    unsigned int bestBoundS = 0;
    unsigned int bestBoundE = 0;
    unsigned int bound = 1;
    unsigned int boundS = 0;
    ++readVecIt;
    for(;readVecIt!=readVecItEnd;++readVecIt, ++bound) {
      // Read pairs should support the same offset and the last kmer should be sufficiently close (allow one mismatch in the last kmer)
      if ((readVecIt->offset == oldOffset) && ((readVecIt->lastKmer - oldKmer) <= TIndex::kmer_size)) {
	if (readVecIt->diag != oldDiag) ++support; // Count only unique reads (unique starting pos);
      } else {
	if (_betterSplit(refSize, refSizeRight, refSizeLeft, oldDiag, oldOffset, skippedLength, initialLength, c.epsilon, support, bestSupport, svType)) {
	  bestSupport = support;
	  bestBoundS = boundS;
	  bestBoundE = bound;
	}
	boundS = bound;
	oldOffset = readVecIt->offset;
	oldDiag = readVecIt->diag;
	oldKmer = readVecIt->lastKmer;
	support = 1;
      }
    }
    if (_betterSplit(refSize, refSizeRight, refSizeLeft, oldDiag, oldOffset, skippedLength, initialLength, c.epsilon, support, bestSupport, svType)) {
      bestSupport = support;
      bestBoundS = boundS;
      bestBoundE = bound;
    }

    // At least the desired number of splitted reads
    if (bestSupport >= c.minimumSplitRead) {
      // Collect all reads for output
      typedef std::vector<OutputRead<TSequence> > TReadOut;
      TReadOut readOut;
      
      // Build the consensus of all split-reads
      typedef std::vector<std::pair<unsigned int, char> > TConsensus;
      TConsensus letters;
      readVecIt = readVec.begin() + bestBoundS;
      readVecItEnd = readVec.begin() + bestBoundE;
      int smallestDiag = readVecIt->diag;
      for(++readVecIt;readVecIt!=readVecItEnd;++readVecIt) {
	if (readVecIt->diag < smallestDiag) smallestDiag = readVecIt->diag;
      }
      readVecIt = readVec.begin() + bestBoundS;
      readVecItEnd = readVec.begin() + bestBoundE;
      for(;readVecIt!=readVecItEnd;++readVecIt) {
	unsigned int pos = readVecIt->diag - smallestDiag;
	OutputRead<TSequence> oR;
	oR.forwardRead = readVecIt->forwardRead;
	oR.readOffset = pos;
	oR.seq = readVecIt->read;
	readOut.push_back(oR);
	if (readVecIt->forwardRead) {
	  typename TSequence::const_iterator p = readVecIt->read.begin();
	  typename TSequence::const_iterator pEnd = readVecIt->read.end();
	  for(; p!=pEnd; ++p) letters.push_back(std::make_pair(pos++, *p));
	} else {
	  typename TSequence::const_iterator p = readVecIt->read.end();
	  typename TSequence::const_iterator pEnd = readVecIt->read.begin();
	  do {
	    --p;
	    unsigned int ch = 4;
	    switch((unsigned int) *p) {
	    case 0: ch = 3; break;
	    case 1: ch = 2; break;
	    case 2: ch = 1; break;
	    case 3: ch = 0; break;
	    }
	    letters.push_back(std::make_pair(pos++, ch));
	  } while (p!=pEnd);
	}
      }
      
      // Sort letters by position
      std::sort(letters.begin(), letters.end());
      typename TConsensus::const_iterator letIt = letters.begin();
      typename TConsensus::const_iterator letItEnd = letters.end();
      typedef std::vector<char> TConsSeq;
      TConsSeq consSeq;
      unsigned int oldPos = letIt->first;
      unsigned int consC[5];
      unsigned int belowCovThres = 0;
      for(unsigned int i = 0;i<5;++i) consC[i] = 0;
      ++consC[(int) letIt->second];
      for(++letIt;letIt!=letItEnd; ++letIt) {
	if (oldPos == letIt->first) ++consC[(int) letIt->second];
	else {
	  char maxI = 0;
	  unsigned int maxCount = consC[0];
	  unsigned int totalCount = consC[0];
	  for(int i = 1;i<5;++i) {
	    totalCount += consC[i];
	    if (consC[i] > maxCount) {
	      maxCount = consC[i];
	      maxI = i;
	    }
	  }
	  if (totalCount >= c.minimumSplitRead) consSeq.push_back(maxI);
	  else if (consSeq.empty()) ++belowCovThres;
	  oldPos = letIt->first;
	  for(unsigned int i = 0;i<5;++i) consC[i] = 0;
	  ++consC[(int) letIt->second];
	}
      }
      char maxI = 0;
      unsigned int maxCount = consC[0];
      unsigned int totalCount = consC[0];
      for(int i = 1;i<5;++i) {
	totalCount += consC[i];
	if (consC[i] > maxCount) {
	  maxCount = consC[i];
	  maxI = i;
	}
      }
      if (totalCount >= c.minimumSplitRead) consSeq.push_back(maxI);
      else if (consSeq.empty()) ++belowCovThres;

      // Debug output
      //typename TConsSeq::const_iterator csSeqIt = consSeq.begin();
      //typename TConsSeq::const_iterator csSeqItEnd = consSeq.end();
      //for(;csSeqIt != csSeqItEnd; ++csSeqIt) std::cout << dna5_decode[(unsigned int) *csSeqIt];
      //std::cout << std::endl;

      // Align the consensus
      TDiag final;
      index.diagonalForwardKmerCount(consSeq, final);
      unsigned int consLen = consSeq.size();

      // Collect the two best diagonals, maybe more for multiple splits?
      TDiag best;
      std::set<unsigned int> usedKmer;
      TDiag leftOver;
      while (final.size()) {
	sort(final.begin(), final.end());
	typename TDiag::const_iterator itD = final.begin();
	typename TDiag::const_iterator itBef = final.begin();
	typename TDiag::const_iterator itDEnd = final.end();
	
	// Count the diagonals 
	unsigned int currentCount =1;
	unsigned int bestCount =0;
	int bestDiag =0;
	for(++itD;itD!=itDEnd; ++itD, ++itBef) {
	  if (itBef->first == itD->first) ++currentCount;
	  else {
	    if (bestCount < currentCount) {
	      bestDiag = itBef->first;
	      bestCount = currentCount;
	    }
	    currentCount = 1;
	  }
	}
	if (bestCount < currentCount) {
	  bestDiag = itBef->first;
	  bestCount = currentCount;
	}

	// Add the diagonal or break if it is not good enough
	if (bestCount < kmerHitCutoff) break;

	// Collect used kmer positions
	usedKmer.clear();
	itD = final.begin();
	itDEnd = final.end();
	unsigned int lastSeenKmer = 0;
	for(;itD!=itDEnd; ++itD) {
	  if (bestDiag == itD->first) {
	    if (itD->second > lastSeenKmer) lastSeenKmer = itD->second;
	    usedKmer.insert(itD->second);
	  }
	}
	best.push_back(std::make_pair(bestDiag, lastSeenKmer));
	//std::cout << bestDiag << ',' << lastSeenKmer << '(' << bestCount << ')' << ';';
	if (best.size() == 2) break;

	// Keep the left-over diagonals
	leftOver.clear();
	itD = final.begin();
	itDEnd = final.end();
	for(;itD!=itDEnd; ++itD) if (usedKmer.find(itD->second) == usedKmer.end()) leftOver.push_back(*itD);
	final.swap(leftOver);
      }
      //std::cout << std::endl;

      // Compare to the reference   
      if (best.size() == 2) {
	TConsSeq ref1;
	TConsSeq ref2;

	int diagBeg = best.begin()->first;
	int diagEnd = (best.begin() + 1)->first;
	if (diagBeg > diagEnd) { int tmp = diagBeg; diagBeg = diagEnd; diagEnd = tmp; }
	
	typename TIndex::TSequence::const_iterator refSeqIt = index.refSequence.begin();
	typename TIndex::TSequence::const_iterator refSeqItEnd = index.refSequence.end();
	int seqSize = 0;
	for(;refSeqIt != refSeqItEnd; ++refSeqIt) {
	  if ((seqSize >= diagBeg) && (seqSize < (int) (diagBeg + consSeq.size()))) ref1.push_back(dna5_encode[(int) *refSeqIt]);
	  if ((seqSize >= diagEnd) && (seqSize < (int) (diagEnd + consSeq.size()))) ref2.push_back(dna5_encode[(int) *refSeqIt]);
	  ++seqSize;
	}

	// Debug output
	//typename TConsSeq::const_iterator csSeqIt = ref1.begin();
	//typename TConsSeq::const_iterator csSeqItEnd = ref1.end();
	//for(;csSeqIt != csSeqItEnd; ++csSeqIt) std::cout << dna5_decode[(unsigned int) *csSeqIt];
	//std::cout << std::endl;
	//csSeqIt = ref2.begin();
	//csSeqItEnd = ref2.end();
	//for(;csSeqIt != csSeqItEnd; ++csSeqIt) std::cout << dna5_decode[(unsigned int) *csSeqIt];
	//std::cout << std::endl;


	// Calculate forward and reverse dynamic programming matrix
	int matchScore = 2;
	int penalty = -3;
	DnaScore<int> sc(matchScore,penalty,penalty,penalty);
	AlignConfig<true, false, false, false> alConf;
	typedef std::vector<int> TScoreMatrix;
	TScoreMatrix mat;
	globalNwAlignmentMatrix(ref1, consSeq, sc, alConf, mat);
	typedef std::pair<int, unsigned int> TScoreCol;
	typedef std::vector<TScoreCol>  TScoreColVector;
	TScoreColVector bestRowScore;
	for(unsigned int row = 0; row<=consSeq.size(); ++row) {
	  int maxRow = mat[row];
	  unsigned int colInd = 0;
	  for(unsigned int col = 1; col<=ref1.size(); ++col) {
	    if (mat[col * (consSeq.size() + 1) + row] > maxRow) {
	      maxRow = mat[col * (consSeq.size() + 1) + row];
	      colInd = col;
	    }
	  }
	  bestRowScore.push_back(std::make_pair(maxRow, colInd));
	}

	// Debug code
	//typedef std::vector<char> TSequence;
	//typedef FastaRecord<std::string, unsigned int, Dna5GapAlphabet, TSequence> TFastaRecord;
	//std::vector<TFastaRecord> align;
	//globalNwAlignment(align, ref1, consSeq, sc, alConf);
	//TSequence::iterator alItTmp = align[0].seq.begin();
	//TSequence::iterator alItTmpEnd = align[0].seq.end();
	//std::cout << std::endl;
	//for(unsigned int i = 0; i<100; ++i) std::cout << (i % 10);
	//std::cout << std::endl;
	//for(;alItTmp != alItTmpEnd; ++alItTmp) std::cout << dna5gap_decode[(int) *alItTmp];
	//std::cout << std::endl;
	//alItTmp = align[1].seq.begin();
	//alItTmpEnd = align[1].seq.end();
	//for(;alItTmp != alItTmpEnd; ++alItTmp) std::cout << dna5gap_decode[(int) *alItTmp];
	//std::cout << std::endl;



	reverseComplement(ref2);
	reverseComplement(consSeq);
	mat.clear();
	globalNwAlignmentMatrix(ref2, consSeq, sc, alConf, mat);
	TScoreColVector bestReverseRowScore;
	bestReverseRowScore.resize(bestRowScore.size());
	for(unsigned int row = 0; row<=consSeq.size(); ++row) {
	  int maxRow = mat[row];
	  unsigned int colInd = 0;
	  for(unsigned int col = 1; col<=ref2.size(); ++col) {
	    if (mat[col * (consSeq.size() + 1) + row] > maxRow) {
	      maxRow = mat[col * (consSeq.size() + 1) + row];
	      colInd = col;
	    }
	  }
	  bestReverseRowScore[bestRowScore.size() - (row + 1)] = std::make_pair(maxRow, (ref2.size() + 1) - (colInd + 1));
	}

	// Debug code
	//align.clear();
	//globalNwAlignment(align, ref2, consSeq, sc, alConf);
	//alItTmp = align[0].seq.begin();
	//alItTmpEnd = align[0].seq.end();
	//std::cout << std::endl;
	//for(;alItTmp != alItTmpEnd; ++alItTmp) std::cout << dna5gap_decode[(int) *alItTmp];
	//std::cout << std::endl;
	//alItTmp = align[1].seq.begin();
	//alItTmpEnd = align[1].seq.end();
	//for(;alItTmp != alItTmpEnd; ++alItTmp) std::cout << dna5gap_decode[(int) *alItTmp];
	//std::cout << std::endl;


	// Get back to the true orientation
	reverseComplement(consSeq);
	reverseComplement(ref2);

	// Find the best alignment split (allowing for microinsertions)
	int maxScore = 0;
	unsigned int leftWalky = 0;
	unsigned int rightWalky = 0;
	unsigned int leftRefWalky = 0;
	unsigned int rightRefWalky = 0;
	TScoreColVector::iterator fIt = bestRowScore.begin();
	TScoreColVector::iterator fItEnd = bestRowScore.end();
	TScoreColVector::iterator rIt = bestReverseRowScore.begin();
	TScoreColVector::iterator rItEnd = bestReverseRowScore.end();
	for(;fIt!=fItEnd; ++fIt, ++rIt) {
	  for(TScoreColVector::iterator rItGo = rIt; rItGo != rItEnd; ++rItGo) {
	    if ((fIt->first + rItGo->first) > maxScore) {
	      maxScore = fIt->first + rItGo->first;
	      leftWalky = fIt - bestRowScore.begin();
	      leftRefWalky = fIt->second;
	      rightWalky = rItGo - bestReverseRowScore.begin() + 1;
	      rightRefWalky = rItGo->second + 1;
	    }
	  }
	}

	// Check split point
	bool invalidAlignment = false;
	if (leftWalky == 0) {
	  invalidAlignment = true;
	  leftWalky = consSeq.size() / 2;
	  rightWalky = leftWalky + 1;
	} else if (rightWalky > consSeq.size()) {
	  invalidAlignment = true;
	  leftWalky = consSeq.size() / 2;
	  rightWalky = leftWalky + 1;
	} else {
	  --leftWalky; --rightWalky;
	  --leftRefWalky; --rightRefWalky;
	}
	if (invalidAlignment) return;
	
	// Get the start and end of the structural variant
	unsigned int finalGapStart, finalGapEnd, predictedLength;
	invalidAlignment=_translateSVCoordinates(refSize, refSizeRight, refSizeLeft, diagBeg, diagEnd, leftRefWalky, rightRefWalky, consLen, sv, finalGapStart, finalGapEnd, predictedLength, svType);

	// Count the final number of aligned reads
	typename TReadOut::const_iterator readIt = readOut.begin();
	typename TReadOut::const_iterator readItEnd = readOut.end();
	int readAlignCount = 0;
	for(;readIt!=readItEnd; ++readIt) {
	  if ((readIt->readOffset + readIt->seq.size() < (unsigned int) rightWalky + belowCovThres + TIndex::kmer_size) || (readIt->readOffset + TIndex::kmer_size > (unsigned int) leftWalky + belowCovThres)) continue;
	  else ++readAlignCount;
	}

	// Calculate quality
	double quality = (double) maxScore / (double) (matchScore * leftWalky + matchScore * (consLen - rightWalky + 1));

	// Valid breakpoint?
	if ((!invalidAlignment) && (readAlignCount >= c.minimumSplitRead) && (leftWalky >= c.minimumFlankSize) && ((consLen - rightWalky) >= c.minimumFlankSize) && (std::abs(((double) predictedLength / (double) initialLength) - 1.0) <= c.epsilon) && (quality >= (double) c.flankQuality / 100.0)) {
	  sv.precise=true;
	  sv.svStart=finalGapStart;
	  sv.svEnd=finalGapEnd;
	  sv.srSupport=readAlignCount;
	  sv.srAlignQuality=quality;
	  typename TConsSeq::const_iterator csSeqIt = consSeq.begin();
	  typename TConsSeq::const_iterator csSeqItEnd = consSeq.end();
	  for(;csSeqIt != csSeqItEnd; ++csSeqIt) sv.consensus += dna5_decode[(unsigned int) *csSeqIt];

	  // Output the consensus
	  /*
	  readIt = readOut.begin();
	  readItEnd = readOut.end();
	  for(;readIt!=readItEnd; ++readIt) {
	    if ( (readIt->readOffset + readIt->seq.size() < (unsigned int) rightWalky + belowCovThres + TIndex::kmer_size) || (readIt->readOffset + TIndex::kmer_size > (unsigned int) leftWalky + belowCovThres) ) continue;
	    unsigned int pos = 0;
	    for(unsigned int securityCheck = 0; (pos < readIt->readOffset) && (securityCheck<10000); ++pos, ++securityCheck) std::cout << ' ';
	    if (readIt->forwardRead) {
	      typename TSequence::const_iterator seqIt = readIt->seq.begin();
	      typename TSequence::const_iterator seqItEnd = readIt->seq.end();
	      for(;seqIt!=seqItEnd; ++seqIt, ++pos) {
		if (pos == rightWalky + belowCovThres) std::cout << "\t";
		std::cout << dna5_decode[(int) *seqIt];
		if (pos == leftWalky + belowCovThres) std::cout << "\t";
	      }
	    } else {
	      typename TSequence::const_iterator seqIt = readIt->seq.end();
	      typename TSequence::const_iterator seqItEnd = readIt->seq.begin();
	      do {
		if (pos == rightWalky + belowCovThres) std::cout << "\t";
		unsigned int ch = 4;
		switch ((unsigned int) *(--seqIt)) {
		case 0: ch = 3; break;
		case 1: ch = 2; break;
		case 2: ch = 1; break;
		case 3: ch = 0; break;
		}
		std::cout << dna5_decode[ch];
		//std::cout << (char) (32 + (int) dna5_decode[ch]);
		if (pos == leftWalky + belowCovThres) std::cout << "\t";
		++pos;
	      } while (seqIt!=seqItEnd);
	    }
	    std::cout << std::endl;
	  }
	  std::cout << "---------------------------------------------" << std::endl;
	  std::cout << sv.chr << "\t" << finalGapStart << "\t" << finalGapEnd << "\t" << (finalGapEnd - finalGapStart) << "\t" << readAlignCount << "\t" << quality << "\t>" << sv.id << "<\t" << std::endl;
	  std::cout << std::endl;
	  */

	  /*
	  // Debug code
	  typename TConsSeq::const_iterator refItT = ref1.begin();
	  typename TConsSeq::const_iterator refItTEnd = ref1.end();
	  std::cout << "PO: ";
	  for(unsigned int i = 0; i<consLen; ++i) {
	    std::cout << (i%10);
	  }
	  std::cout << std::endl;
	  std::cout << "RL: ";
	  for (;refItT!=refItTEnd; ++refItT) {
	    std::cout << dna5_decode[(int) *refItT];
	  }
	  std::cout << std::endl;
	  std::cout << "RR: ";
	  refItT = ref2.begin();
	  refItTEnd = ref2.end();
	  for (;refItT!=refItTEnd; ++refItT) {
	    std::cout << dna5_decode[(int) *refItT];
	  }
	  std::cout << std::endl;
	  std::cout << "CO: ";
	  refItT = consSeq.begin();
	  refItTEnd = consSeq.end();
	  for (;refItT!=refItTEnd; ++refItT) {
	    std::cout << dna5_decode[(int) *refItT];
	  }
	  std::cout << std::endl;  
	  std::cout << std::endl;
	  std::cout << "ML: ";
	  typename TIncSum::const_iterator leftIt = leftSc.begin();
	  typename TIncSum::const_iterator leftItEnd = leftSc.end();
	  for (;leftIt!=leftItEnd; ++leftIt) {
	    std::cout << (*leftIt % 10);
	  }
	  std::cout << std::endl;  
	  std::cout << "MR: ";
	  typename TIncSum::const_iterator rightIt = rightSc.begin();
	  typename TIncSum::const_iterator rightItEnd = rightSc.end();
	  for (;rightIt!=rightItEnd; ++rightIt) {
	    std::cout << (*rightIt % 10);
	  }
	  std::cout << std::endl;  
	  std::cout << "SU: ";
	  leftIt = leftSc.begin();
	  rightIt = rightSc.begin();
	  for (;rightIt!=rightItEnd; ++leftIt, ++rightIt) {
	    std::cout << ((*rightIt + *leftIt) % 10);
	  }
	  std::cout << std::endl;  
	  */
	}
      }
    }
  }
}


// Deletions
inline std::string
_addID(SVType<DeletionTag>) {
  return "DEL";
}

// Duplications
inline std::string
_addID(SVType<DuplicationTag>) {
  return "DUP";
}

template<typename TConfig, typename TStructuralVariantRecord, typename TTag>
inline void
vcfOutput(TConfig const& c, std::vector<TStructuralVariantRecord> const& dels, SVType<TTag> svType) 
{
  // Output all deletions
  std::ofstream del_file(c.outfile.string().c_str());

  // Print vcf header
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  boost::gregorian::date today = now.date();
  del_file << "##fileformat=VCFv4.1" << std::endl;
  del_file << "##fileDate=" << boost::gregorian::to_iso_string(today) << std::endl;
  del_file << "##ALT=<ID=DEL,Description=\"Deletion\">" << std::endl;
  del_file << "##ALT=<ID=DUP,Description=\"Duplication\">" << std::endl;
  del_file << "##FILTER=<ID=LowQual,Description=\"PE support below 3 or mapping quality below 20.\">" << std::endl;
  del_file << "##INFO=<ID=CIEND,Number=2,Type=Integer,Description=\"PE confidence interval around END\">" << std::endl;
  del_file << "##INFO=<ID=CIPOS,Number=2,Type=Integer,Description=\"PE confidence interval around POS\">" << std::endl;
  del_file << "##INFO=<ID=END,Number=1,Type=Integer,Description=\"End position of the structural variant\">" << std::endl;
  del_file << "##INFO=<ID=PE,Number=1,Type=Integer,Description=\"Paired-end support of the structural variant\">" << std::endl;
  del_file << "##INFO=<ID=MAPQ,Number=1,Type=Integer,Description=\"Median mapping quality of paired-ends\">" << std::endl;
  del_file << "##INFO=<ID=SR,Number=1,Type=Integer,Description=\"Split-read support\">" << std::endl;
  del_file << "##INFO=<ID=SRQ,Number=1,Type=Float,Description=\"Split-read consensus alignment quality\">" << std::endl;
  del_file << "##INFO=<ID=CONSENSUS,Number=1,Type=String,Description=\"Split-read consensus sequence\">" << std::endl;
  del_file << "##INFO=<ID=IMPRECISE,Number=0,Type=Flag,Description=\"Imprecise structural variation\">" << std::endl;
  del_file << "##INFO=<ID=PRECISE,Number=0,Type=Flag,Description=\"Precise structural variation\">" << std::endl;
  del_file << "##INFO=<ID=SVLEN,Number=1,Type=Integer,Description=\"Length of the SV\">" << std::endl;
  del_file << "##INFO=<ID=SVTYPE,Number=1,Type=String,Description=\"Type of structural variant\">" << std::endl;
  del_file << "##INFO=<ID=SVMETHOD,Number=1,Type=String,Description=\"Type of approach used to detect SV\">" << std::endl;
  del_file << "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO" << std::endl;

  typedef std::vector<TStructuralVariantRecord> TSVs;
  typename TSVs::const_iterator delIter = dels.begin();
  typename TSVs::const_iterator delIterEnd = dels.end();
  for(;delIter!=delIterEnd;++delIter) {
    std::string filterField="PASS";
    if ((delIter->peSupport < 3) || (delIter->peMapQuality < 20)) {
      filterField="LowQual";
    }

    std::stringstream id;
    id << _addID(svType) << std::setw(8) << std::setfill('0') << delIter->id;
    del_file << delIter->chr << "\t" << delIter->svStart << "\t" << id.str() << "\tN\t<" << _addID(svType) << ">\t.\t" <<  filterField << "\t";
    // Add info fields
    if (delIter->precise) del_file << "PRECISE;";
    else del_file << "IMPRECISE;";
    del_file << "CIEND=" << -delIter->wiggle << "," << delIter->wiggle << ";CIPOS=" << -delIter->wiggle << "," << delIter->wiggle << ";";
    del_file << "SVTYPE=" << _addID(svType) << ";";
    del_file << "SVMETHOD=EMBL.DELLY;";
    del_file << "END=" << delIter->svEnd << ";";
    del_file << "SVLEN=" << (delIter->svEnd - delIter->svStart) << ";";
    del_file << "PE=" << delIter->peSupport << ";";
    del_file << "MAPQ=" << delIter->peMapQuality << ";";
    if (delIter->precise)  {
      del_file << "SR=" << delIter->srSupport << ";";
      del_file << "SRQ=" << delIter->srAlignQuality << ";";
      del_file << "CONSENSUS=" << delIter->consensus << ";";
    }
    del_file << std::endl;
  }

  del_file.close();
}


// Deletions
template<typename TSeq, typename TBamRecord>
inline std::string
_getSVRef(TSeq const* const ref, TBamRecord const& bamRec, SVType<DeletionTag>) {
  return boost::to_upper_copy(std::string(ref + bamRec.svStartBeg, ref + bamRec.svStartEnd)) + boost::to_upper_copy(std::string(ref + bamRec.svEndBeg, ref + bamRec.svEndEnd));
}

// Duplications
template<typename TSeq, typename TBamRecord>
inline std::string
_getSVRef(TSeq const* const ref, TBamRecord const& bamRec, SVType<DuplicationTag>) {
  return boost::to_upper_copy(std::string(ref + bamRec.svEndBeg, ref + bamRec.svEndEnd)) + boost::to_upper_copy(std::string(ref + bamRec.svStartBeg, ref + bamRec.svStartEnd));
}

template<typename TConfig, typename TStructuralVariantRecord, typename TTag>
inline bool
findPutativeSplitReads(TConfig const& c, std::vector<TStructuralVariantRecord>& svs,  SVType<TTag> svType) 
{
  typedef std::vector<TStructuralVariantRecord> TSVs;

  // Parse reference information
  BamTools::BamReader reader;
  if ( ! reader.Open(c.files[0].string())) return -1;
  BamTools::RefVector references = reader.GetReferenceData();

  // Collect good quality single-anchored reads from all samples
  typedef std::map<std::size_t, std::string> TSingleMap;
  TSingleMap singleAnchored;
  unsigned int maxSingleAnchoredReads=10000000;
  for(unsigned int file_c = 0; ((file_c < c.files.size()) && (singleAnchored.size() <= maxSingleAnchoredReads)); ++file_c) {
    BamTools::BamReader reader;
    if ( ! reader.Open(c.files[file_c].string()) ) return -1;
    BamTools::BamAlignment al;
    while( reader.GetNextAlignment(al) ) {
      // Read unmapped
      if (al.AlignmentFlag & 0x0004) {
	// Paired-end partner is mapped
	if ((al.AlignmentFlag & 0x0001) && !(al.AlignmentFlag & 0x0008) && !(al.AlignmentFlag & 0x0100) && !(al.AlignmentFlag & 0x0200) && !(al.AlignmentFlag & 0x0400) && (al.Position!=al.MatePosition)) {
	  // Check qualities
	  std::string::const_iterator qIter= al.Qualities.begin();
	  int meanQual = 0;
	  for(;qIter!=al.Qualities.end();++qIter) meanQual+=static_cast<int>(*qIter);
	  meanQual /= al.Length;
	  meanQual -=33;
	  if (meanQual >= 20) {
	    boost::hash<std::string> hashStr;
	    singleAnchored.insert(std::make_pair(hashStr(al.Name), al.QueryBases));
	  }
	}
      }
    }
  }
  //std::cout << "Num. single-anchored reads: " << singleAnchored.size() << std::endl;

  // Parse genome
  unsigned int totalSplitReadsAligned = 0;
  kseq_t *seq;
  int l;
  gzFile fp = gzopen(c.genome.string().c_str(), "r");
  seq = kseq_init(fp);
  std::cout << "Split-read alignment" << std::endl;
  boost::progress_display show_progress( references.size() );
  while ((l = kseq_read(seq)) >= 0) {
    // Find reference index
    ++show_progress;
    BamTools::RefVector::const_iterator itRef = references.begin();
    for(unsigned int refIndex=0;itRef!=references.end();++itRef, ++refIndex) {
      if (seq->name.s == references[refIndex].RefName) {
	// Dummy placeholders for softclips
	std::vector<int> clipSizes;
	std::vector<int> readPositions;
	std::vector<int> genomePositions;

	// Iterate all deletions on this chromosome
	typename TSVs::iterator svIt = svs.begin();
	typename TSVs::iterator svItEnd = svs.end();
	for(;svIt!=svItEnd; ++svIt) {
	  if (svIt->chr == references[refIndex].RefName) {
	    // Get the SV reference
	    std::string svRefStr = _getSVRef(seq->seq.s, *svIt, svType);
	    std::set<std::string> splitReadSet;
	    unsigned int splitReadSetMaxSize = 1000;

	    // Find putative split reads in all samples
	    for(unsigned int file_c = 0; ((file_c < c.files.size()) && (splitReadSet.size() < splitReadSetMaxSize)); ++file_c) {
	      // Initialize bam file
	      BamTools::BamReader reader;
	      if ( ! reader.Open(c.files[file_c].string()) ) return -1;
	      reader.LocateIndex();
	      if ( !reader.HasIndex() ) return -1;

	      BamTools::BamAlignment al;
	      if ( reader.SetRegion(refIndex, (svIt->svStartBeg + svIt->svStart)/2, refIndex, (svIt->svStart + svIt->svStartEnd)/2 ) ) {
		while ((reader.GetNextAlignment(al)) && (splitReadSet.size() < splitReadSetMaxSize)) {
		  if (!(al.AlignmentFlag & 0x0100) && !(al.AlignmentFlag & 0x0200) && !(al.AlignmentFlag & 0x0400)) {
		    // Single-anchored read?
		    if ((al.AlignmentFlag & 0x0001) && !(al.AlignmentFlag & 0x0004) && (al.AlignmentFlag & 0x0008)) {
		      boost::hash<std::string> hashStr;
		      TSingleMap::const_iterator singleIt = singleAnchored.find(hashStr(al.Name));
		      if (singleIt!=singleAnchored.end()) {
			splitReadSet.insert(singleIt->second);
		      }
		    }
		    // Clipped read or large edit distance
		    unsigned int editDistance = 0;
		    al.GetTag("NM", editDistance);
		    if ((editDistance>10) || (al.GetSoftClips(clipSizes, readPositions, genomePositions, false))) {
		      splitReadSet.insert(al.QueryBases);
		    }
		  }
		}
	      }
	      if ( reader.SetRegion(refIndex, (svIt->svEndBeg + svIt->svEnd)/2, refIndex, (svIt->svEnd + svIt->svEndEnd)/2 ) ) {
		while ((reader.GetNextAlignment(al)) && (splitReadSet.size() < splitReadSetMaxSize)) {
		  if (!(al.AlignmentFlag & 0x0100) && !(al.AlignmentFlag & 0x0200) && !(al.AlignmentFlag & 0x0400)) {
		    // Single-anchored read?
		    if ((al.AlignmentFlag & 0x0001) && !(al.AlignmentFlag & 0x0004) && (al.AlignmentFlag & 0x0008)) {
		      boost::hash<std::string> hashStr;
		      TSingleMap::const_iterator singleIt = singleAnchored.find(hashStr(al.Name));
		      if (singleIt!=singleAnchored.end()) {
			splitReadSet.insert(singleIt->second);
		      }
		    }
		    // Clipped read or large edit distance
		    unsigned int editDistance = 0;
		    al.GetTag("NM", editDistance);
		    if ((editDistance>10) || (al.GetSoftClips(clipSizes, readPositions, genomePositions, false))) {
		      splitReadSet.insert(al.QueryBases);
		    }
		  }
		}
	      }
	    }
	    //std::cout << "Num. split reads: " << splitReadSet.size() << std::endl;

	    // Compare the split-reads against the SV reference
	    if (!splitReadSet.empty()) {
	      totalSplitReadsAligned += splitReadSet.size();

	      // Search true split in candidates
	      searchSplit(c, *svIt, svRefStr, splitReadSet, svType);
	    }
	  }
	}
      }
    }
  }
  kseq_destroy(seq);
  gzclose(fp);

  return (totalSplitReadsAligned>0);
}


// Deletions
template<typename TISize>
inline bool
_acceptedInsertSize(TISize maxNormalISize, TISize, TISize iSize, SVType<DeletionTag>) {
  return (maxNormalISize > iSize);
}

// Duplications
template<typename TISize>
inline bool
_acceptedInsertSize(TISize, TISize median, TISize iSize, SVType<DuplicationTag>) {
  // Exclude the chimeras in mate-pair libraries
  return !((median<1000) || ((median>=1000) && (iSize >=1000)));
}

// Other SV Types
template<typename TISize, typename TTag>
inline bool
_acceptedInsertSize(TISize, TISize, TISize, SVType<TTag>) {
  return false;
}

// Deletions
template<typename TOrientation>
inline bool
_acceptedOrientation(TOrientation def, TOrientation lib, SVType<DeletionTag>) {
  return (def != lib);
}

// Duplications
template<typename TOrientation>
inline bool
_acceptedOrientation(TOrientation def, TOrientation lib, SVType<DuplicationTag>) {
  if (def==0) return (lib != 1);
  else if (def==1) return (lib!=0);
  else if (def==2) return (lib!=3);
  else if (def==3) return (lib!=2);
  else return true;
}

// Other SV Types
template<typename TOrientation, typename TTag>
inline bool
_acceptedOrientation(TOrientation def, TOrientation lib, SVType<TTag>) {
  return false;
}

// Deletions
template<typename TSize, typename TISize>
inline bool
_pairsDisagree(TSize pair1Min, TSize pair1Max, TSize pair1ReadLength, TISize pair1maxNormalISize, TSize pair2Min, TSize pair2Max, TSize pair2ReadLength, TISize pair2maxNormalISize, SVType<DeletionTag>) {
  if ((pair2Min + pair2ReadLength - pair1Min) > pair1maxNormalISize) return true;
  if ((pair2Max < pair1Max) && ((pair1Max + pair1ReadLength - pair2Max) > pair1maxNormalISize)) return true;
  if ((pair2Max >= pair1Max) && ((pair2Max + pair2ReadLength - pair1Max) > pair2maxNormalISize)) return true;
  return false;
}

// Duplications
template<typename TSize, typename TISize>
inline bool
_pairsDisagree(TSize pair1Min, TSize pair1Max, TSize pair1ReadLength, TISize pair1maxNormalISize, TSize pair2Min, TSize pair2Max, TSize pair2ReadLength, TISize pair2maxNormalISize, SVType<DuplicationTag>) {
  if ((pair2Min + pair2ReadLength - pair1Min) > pair2maxNormalISize) return true;
  if ((pair2Max < pair1Max) && ((pair1Max + pair1ReadLength - pair2Max) > pair2maxNormalISize)) return true;
  if ((pair2Max >= pair1Max) && ((pair2Max + pair2ReadLength - pair1Max) > pair1maxNormalISize)) return true;
  return false;
}

// Initialize clique, deletions
template<typename TBamRecordIterator, typename TSize>
inline void
_initClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DeletionTag>) {
  svStart = std::min(el->Position + el->Length, el->MatePosition + el->Length);
  svEnd = std::max(el->Position, el->MatePosition);
  wiggle =  abs(el->Position - el->MatePosition) + el->Length - el->maxNormalISize -(svEnd -svStart);
}

// Initialize clique, duplications
template<typename TBamRecordIterator, typename TSize>
inline void
_initClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DuplicationTag>) {
  svStart = std::min(el->Position, el->MatePosition);
  svEnd = std::max(el->Position + el->Length, el->MatePosition + el->Length);
  wiggle =  abs(el->Position - el->MatePosition) - el->Length + el->maxNormalISize -(svEnd -svStart);
}

// Update clique, deletions
template<typename TBamRecordIterator, typename TSize>
inline bool 
_updateClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DeletionTag>) 
{
  TSize newSvStart = std::max(svStart, std::min(el->Position + el->Length, el->MatePosition + el->Length));
  TSize newSvEnd = std::min(svEnd, std::max(el->Position, el->MatePosition));
  TSize newWiggle = abs(el->Position - el->MatePosition) + el->Length - el->maxNormalISize -(newSvEnd - newSvStart);
  TSize wiggleChange = wiggle + (svEnd-svStart) - (newSvEnd - newSvStart);
  if (wiggleChange > newWiggle) newWiggle=wiggleChange;

  // Does the new deletion size agree with all pairs
  if ((newSvStart < newSvEnd) && (newWiggle<=0)) {
    svStart = newSvStart;
    svEnd = newSvEnd;
    wiggle = newWiggle;
    return true;
  }
  return false;
}

// Update clique, duplications
template<typename TBamRecordIterator, typename TSize>
inline bool 
_updateClique(TBamRecordIterator const& el, TSize& svStart, TSize& svEnd, TSize& wiggle, SVType<DuplicationTag>) 
{
  TSize newSvStart = std::min(svStart, std::min(el->Position, el->MatePosition));
  TSize newSvEnd = std::max(svEnd, std::max(el->Position + el->Length, el->MatePosition + el->Length));
  TSize newWiggle = abs(el->Position - el->MatePosition) - el->Length + el->maxNormalISize -(newSvEnd - newSvStart);
  TSize wiggleChange = wiggle - ((newSvEnd - newSvStart) - (svEnd-svStart));
  if (wiggleChange < newWiggle) newWiggle=wiggleChange;

  // Does the new duplication size agree with all pairs
  if ((newSvStart < newSvEnd) && (newWiggle>=0)) {
    svStart = newSvStart;
    svEnd = newSvEnd;
    wiggle = newWiggle;
    return true;
  }
  return false;
}


template<typename TSVType>
inline int run(Config const& c, TSVType svType) {
  // Collect all promising structural variants
  typedef std::vector<StructuralVariantRecord> TVariants;
  TVariants svs;

  // Clique id counter
  unsigned int clique_count = 0;

  // Create library objects
  typedef std::map<std::string, LibraryInfo> TLibraryMap;
  typedef std::map<std::string, TLibraryMap> TSampleLibrary;
  TSampleLibrary sampleLib;
  int overallMaxISize = 0;

  // Scan libraries first
  BamTools::RefVector references;
  for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
    // Get a sample name
    std::string sampleName(c.files[file_c].stem().string());

    // Check that all input bam files exist
    BamTools::BamReader reader;
    if ( ! reader.Open(c.files[file_c].string()) ) {
      std::cerr << "Could not open input bam file: " << c.files[file_c].string() << std::endl;
      reader.Close();
      return -1;
    }
    
    // Check that all input bam files are indexed
    reader.LocateIndex();
    if ( !reader.HasIndex() ) {
      std::cerr << "Missing bam index file: " << c.files[file_c].string() << std::endl;
      reader.Close();
      return -1;
    }

    // Get references
    if (file_c==0) references = reader.GetReferenceData();

    // Get library parameters and overall maximum insert size
    TLibraryMap libInfo;
    getLibraryParams(c.files[file_c], libInfo, c.percentAbnormal, c.madCutoff);
    TLibraryMap::const_iterator libIter=libInfo.begin();
    for(;libIter!=libInfo.end();++libIter) 
      if (libIter->second.maxNormalISize > overallMaxISize) overallMaxISize = libIter->second.maxNormalISize;
    sampleLib.insert(std::make_pair(sampleName, libInfo));
  }


  // Exclude chromosomes
  typedef std::map<std::string, unsigned int> TMapChr;
  TMapChr mapChr;
  BamTools::RefVector::const_iterator itRef = references.begin();
  for(unsigned int i = 0;itRef!=references.end();++itRef, ++i) mapChr.insert(std::make_pair(itRef->RefName, i));
  std::vector<bool> validChr;
  validChr.resize(references.size());
  std::fill(validChr.begin(), validChr.end(), true);
  if (boost::filesystem::exists(c.exclude) && boost::filesystem::is_regular_file(c.exclude) && boost::filesystem::file_size(c.exclude)) {
    std::ifstream chrFile(c.exclude.string().c_str());
    if (chrFile.is_open()) {
      std::string chrFromFile;
      while (chrFile.good()) {
	getline(chrFile, chrFromFile);
	TMapChr::const_iterator mapChrIt = mapChr.find(chrFromFile);
	if (mapChrIt != mapChr.end()) validChr[mapChrIt->second]=false;
      }
      chrFile.close();
    }
  }

  // Process chromosome by chromosome
  std::cout << "Paired-end clustering" << std::endl;
  boost::progress_display show_progress( (references.end() - references.begin()) );
  itRef = references.begin();
  for(int refIndex=0;((itRef!=references.end()) && (c.peMode));++itRef, ++refIndex) {
    ++show_progress;
    if (!validChr[refIndex]) continue;
      
    // Create bam alignment record vector
    typedef std::vector<BamAlignRecord> TBamRecord;
    TBamRecord bamRecord;

    // Iterate all samples
    for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
      // Get a sample name
      std::string sampleName(c.files[file_c].stem().string());
      TSampleLibrary::iterator sampleIt=sampleLib.find(sampleName);

      // Initialize bam file
      BamTools::BamReader reader;
      if ( ! reader.Open(c.files[file_c].string()) ) return -1;
      reader.LocateIndex();
      if ( !reader.HasIndex() ) return -1;

      // Unique pairs for the given sample
      typedef std::set<Hit> TUniquePairs;
      TUniquePairs unique_pairs;

      // Read alignments and hash qualities
      uint16_t* qualities = new uint16_t[(int)boost::math::pow<28>(2)];
      uint16_t* qualitiesEnd = qualities + (int) boost::math::pow<28>(2);
      std::fill(qualities, qualitiesEnd, 0);
      BamTools::BamAlignment al;
      if ( reader.Jump(refIndex, 0) ) {
	while( reader.GetNextAlignment(al) ) {
	  if (al.RefID!=refIndex) break; // Stop when we hit the next chromosome
	  if ((al.AlignmentFlag & 0x0001) && !(al.AlignmentFlag & 0x0004) && !(al.AlignmentFlag & 0x0008) && !(al.AlignmentFlag & 0x0100) && !(al.AlignmentFlag & 0x0200) && !(al.AlignmentFlag & 0x0400) && (al.RefID==al.MateRefID) && (al.MapQuality >= c.minMapQual) && (al.Position!=al.MatePosition)) {
	    // Is this a discordantly mapped paired-end?
	    std::string rG = "DefaultLib";
	    al.GetTag("RG", rG);
	    TLibraryMap::iterator libIt=sampleIt->second.find(rG);
	    if (libIt->second.median == 0) continue; // Single-end library
	    if (_acceptedInsertSize(libIt->second.maxNormalISize, libIt->second.median, abs(al.InsertSize), svType)) continue;  // Normal paired-end (for deletions only)
	    if (_acceptedOrientation(libIt->second.defaultOrient, getStrandIndependentOrientation(al), svType)) continue;  // Orientation disagrees with SV type

	    // Get or store the mapping quality for the partner
	    if (al.Position<al.MatePosition) {
	      // Hash the quality
	      boost::hash<std::string> hashStr;
	      unsigned int index=((hashStr(al.Name) % (int)boost::math::pow<4>(2))<<24) + ((al.Position % (int)boost::math::pow<12>(2))<<12) + (al.MatePosition % (int)boost::math::pow<12>(2));
	      qualities[index]=al.MapQuality;
	    } else {
	      // Get the two mapping qualities
	      boost::hash<std::string> hashStr;
	      unsigned int index=((hashStr(al.Name) % (int)boost::math::pow<4>(2))<<24) + ((al.MatePosition % (int)boost::math::pow<12>(2))<<12) + (al.Position % (int)boost::math::pow<12>(2));
	      uint16_t pairQuality = std::min(qualities[index], al.MapQuality);
	      //std::cout << al.Name << ',' << qualities[index] << ',' << mateQuality << ',' << pairQuality << ',' << std::endl;
	      qualities[index]=0;

	      // Store the paired-end
	      Hit hitPos(al);
	      TUniquePairs::const_iterator pos = unique_pairs.begin();
	      bool inserted;
	      boost::tie(pos, inserted) = unique_pairs.insert(hitPos);
	      if (inserted) {
		bamRecord.push_back(BamAlignRecord(al, pairQuality, libIt->second.median, libIt->second.mad, libIt->second.maxNormalISize));
		++libIt->second.unique_pairs;
	      } else {
		++libIt->second.non_unique_pairs;
	      }
	    }
	  }
	}
      }
      delete [] qualities;
    }
    
    // Sort BAM records according to position
    std::sort(bamRecord.begin(), bamRecord.end(), SortBamRecords<BamAlignRecord>());

    // Define an undirected graph g
    typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS, TBamRecord::const_iterator, boost::property<boost::edge_weight_t, unsigned short> > Graph;
    Graph g;
      
    // Define the reverse map
    typedef boost::graph_traits<Graph>::vertex_descriptor Vertex;
    typedef std::map<TBamRecord::const_iterator, Vertex> TNameVertexMap;
    TNameVertexMap nameFrag;
      
    // Define the edge property map
    typedef boost::property_map<Graph, boost::edge_weight_t>::type edge_map_t;
    edge_map_t weightMap = get(boost::edge_weight, g);
      
    // Iterate the chromosome range
    TBamRecord::const_iterator vecBeg = bamRecord.begin();
    TBamRecord::const_iterator vecEnd = bamRecord.end();
    for(;vecBeg!=vecEnd; ++vecBeg) {
      int32_t const minCoord = std::min(vecBeg->Position, vecBeg->MatePosition);
      int32_t const maxCoord = std::max(vecBeg->Position, vecBeg->MatePosition);
      TBamRecord::const_iterator vecNext = vecBeg + 1;
      for(; ((vecNext != vecEnd) && (abs(std::min(vecNext->Position, vecNext->MatePosition) + vecNext->Length - minCoord) <= overallMaxISize)) ; ++vecNext) {
	// Check combinability of pairs
	if (_pairsDisagree(minCoord, maxCoord, vecBeg->Length, vecBeg->maxNormalISize, std::min(vecNext->Position, vecNext->MatePosition), std::max(vecNext->Position, vecNext->MatePosition), vecNext->Length, vecNext->maxNormalISize, svType)) continue;
	
	TNameVertexMap::iterator pos;
	bool inserted;
	
	// Add vertex 1
	Vertex u;
	boost::tie(pos, inserted) = nameFrag.insert(std::make_pair(vecBeg, Vertex()));
	if (inserted) {
	  u = add_vertex(g);
	  pos->second = u;
	  g[u]=vecBeg;
	} else {
	  u = pos->second;
	}
	
	// Add vertex 2
	Vertex v;
	boost::tie(pos, inserted) = nameFrag.insert(std::make_pair(vecNext, Vertex()));
	if (inserted) {
	  v = add_vertex(g);
	  pos->second = v;
	  g[v]=vecNext;
	} else {
	  v = pos->second;
	}
	
	// Add the edge
	if ((out_degree(u, g) <= c.graphPruning) || (out_degree(v, g) <= c.graphPruning)) {
	  boost::graph_traits<Graph>::edge_descriptor e;
	  tie(e, inserted) = add_edge(u,v, g);
	  if (inserted) {
	    int locEdgeWeight=abs( abs( (std::min(vecNext->Position, vecNext->MatePosition) - minCoord) - (std::max(vecNext->Position, vecNext->MatePosition) - maxCoord) ) - abs(vecBeg->Median - vecNext->Median) );
	    weightMap[e] = (locEdgeWeight > 30000) ? 30000 : locEdgeWeight;
	  }
	}
      }
    }
    nameFrag.clear();

    // Compute the connected components
    std::vector<int> my_comp(num_vertices(g));
    int numComp = boost::connected_components(g, &my_comp[0]);
    
    // Count the number of vertices for each component
    std::vector<unsigned int> compSize(numComp);
    boost::graph_traits<Graph>::vertex_iterator vIt, vItEnd;
    for(boost::tie(vIt, vItEnd) = boost::vertices(g); vIt != vItEnd; ++vIt) {
      ++compSize[my_comp[*vIt]];
    }

    // Iterate each component
    for(int compIt = 0; compIt < numComp; ++compIt) {
      if (compSize[compIt]<2) continue;
      typedef boost::graph_traits<Graph>::vertex_descriptor TVertexDescriptor;
      typedef boost::graph_traits<Graph>::edge_descriptor TEdgeDescriptor;
      typedef std::vector< std::pair<unsigned short, TEdgeDescriptor> > TWeightEdge;
      TWeightEdge wEdge;
      boost::graph_traits<Graph>::edge_iterator eIt, eItEnd;
      for(boost::tie(eIt, eItEnd) = boost::edges(g); eIt != eItEnd; ++eIt) {
	if ((my_comp[boost::source(*eIt, g)] == compIt) && (my_comp[boost::source(*eIt, g)] == my_comp[boost::target(*eIt,g)])) {
	  wEdge.push_back(std::make_pair(weightMap[*eIt], *eIt));
	}
      }
      
      // Sort edges by weight
      std::sort(wEdge.begin(), wEdge.end());
      
      // Find a large clique
      TWeightEdge::const_iterator itWEdge = wEdge.begin();
      TWeightEdge::const_iterator itWEdgeEnd = wEdge.end();
      typedef std::set<TVertexDescriptor> TCliqueMembers;
      TCliqueMembers clique;
      int svStart, svEnd, wiggle;
      _initClique(g[boost::source(itWEdge->second, g)], svStart, svEnd, wiggle, svType);
      if (svStart >= svEnd)  continue;
      clique.insert(boost::source(itWEdge->second, g));
      
      // Grow the clique from the seeding edge
      bool cliqueGrow=true;
      while ((cliqueGrow) && (clique.size() < compSize[compIt])) {
	TCliqueMembers::const_iterator cliqueEnd = clique.end();
	itWEdge = wEdge.begin();
	cliqueGrow = false;
	for(;(!cliqueGrow) && (itWEdge != itWEdgeEnd);++itWEdge) {
	  TVertexDescriptor v;
	  if ( (clique.find(boost::source(itWEdge->second, g)) == cliqueEnd) && (clique.find(boost::target(itWEdge->second, g)) != cliqueEnd)) v = boost::source(itWEdge->second, g);
	  else if ( (clique.find(boost::source(itWEdge->second, g)) != cliqueEnd) && (clique.find(boost::target(itWEdge->second, g)) == cliqueEnd)) v = boost::target(itWEdge->second, g);
	  else continue;
	  boost::graph_traits<Graph>::adjacency_iterator vi, vi_end;
	  unsigned int cliqSize = 0;
	  for(boost::tie(vi, vi_end) = boost::adjacent_vertices(v, g); vi != vi_end; ++vi)
	    if (clique.find(*vi) != cliqueEnd) ++cliqSize;
	  if (cliqSize == clique.size()) {
	    cliqueGrow = _updateClique(g[v], svStart, svEnd, wiggle, svType);
	    if (cliqueGrow) clique.insert(v);
	  }
	}
      }

      if ((clique.size()>1) && ((svEnd - svStart) >= 100)) {
	StructuralVariantRecord svRec;
	svRec.chr = references[refIndex].RefName;
	svRec.svStartBeg = std::max((int) svStart - overallMaxISize, 0);
	svRec.svStart = svStart +1;
	svRec.svStartEnd = svStart + overallMaxISize;
	svRec.svEndBeg = std::max((int) svEnd - overallMaxISize, 0);
	svRec.svEnd = svEnd+1;
	svRec.svEndEnd = svEnd + overallMaxISize;
	svRec.peSupport = clique.size();
	svRec.wiggle = abs(wiggle);
	std::vector<uint16_t> mapQV;
	typename TCliqueMembers::const_iterator itC = clique.begin();
	for(;itC!=clique.end();++itC) mapQV.push_back(g[*itC]->MapQuality);
	std::sort(mapQV.begin(), mapQV.end());
	svRec.peMapQuality = mapQV[mapQV.size()/2];
	if (svRec.svStartEnd > svRec.svEndBeg) {
	  unsigned int midPointDel = ((svRec.svEnd - svRec.svStart) / 2) + svRec.svStart;
	  svRec.svStartEnd = midPointDel -1;
	  svRec.svEndBeg = midPointDel;
	}
	svRec.id = clique_count++;
	svRec.srSupport=0;
	svRec.srAlignQuality=0;
	svRec.precise=false;
	svs.push_back(svRec);
      }
    }
  }
   
  // Output library statistics
  std::cout << "Library statistics" << std::endl;
  TSampleLibrary::const_iterator sampleIt=sampleLib.begin();
  for(;sampleIt!=sampleLib.end();++sampleIt) {
    std::cout << "Sample: " << sampleIt->first << std::endl;
    TLibraryMap::const_iterator libIt=sampleIt->second.begin();
    for(;libIt!=sampleIt->second.end();++libIt) {
      std::cout << "RG: ID=" << libIt->first << ",Median=" << libIt->second.median << ",MAD=" << libIt->second.mad << ",Orientation=" << (int) libIt->second.defaultOrient << ",InsertSizeCutoff=" << libIt->second.maxNormalISize << ",DuplicateDiscordantPairs=" << libIt->second.non_unique_pairs << ",UniqueDiscordantPairs=" << libIt->second.unique_pairs << std::endl;
    }
  }

  // No PEM mode
  if (!c.peMode) {
    std::cout << "No paired-end analysis!" << std::endl;

    // Read deletion intervals
    typedef Record<std::string, unsigned int, unsigned int, std::string, void, void, void, void, void, void, void, void> TRecord;
    Memory_mapped_file map_file(c.variants.string().c_str());
    char buffer[Memory_mapped_file::MAX_LINE_LENGTH];
    while (map_file.left_bytes() > 0) {
      map_file.read_line(buffer);
      Tokenizer token(buffer, Memory_mapped_file::MAX_LINE_LENGTH);
      TRecord line;
      addF0(token, line);
      addF1(token, line);
      addF2(token, line);
      addF3(token, line);

      // Store as structural variant record
      StructuralVariantRecord svRec;
      svRec.chr = line.f0;
      svRec.svStartBeg = std::max((int) line.f1 - overallMaxISize, 0);
      svRec.svStart = line.f1;
      svRec.svStartEnd = line.f1 + overallMaxISize;
      svRec.svEndBeg = std::max((int) line.f2 - overallMaxISize, 0);
      svRec.svEnd = line.f2;
      svRec.svEndEnd = line.f2 + overallMaxISize;
      svRec.wiggle = overallMaxISize;
      if (svRec.svStartEnd > svRec.svEndBeg) {
	unsigned int midPointDel = ((svRec.svEnd - svRec.svStart) / 2) + svRec.svStart;
	svRec.svStartEnd = midPointDel -1;
	svRec.svEndBeg = midPointDel;
      }
      svRec.peSupport = 3;
      svRec.peMapQuality = 20;
      svRec.id = clique_count++;
      svRec.srSupport=0;
      svRec.srAlignQuality=0;
      svRec.precise=false;
      svs.push_back(svRec);
    }
    map_file.close();
    std::cout << "Created " << clique_count << " PE deletions." << std::endl;
  }

  // Split-read search
  if (boost::filesystem::exists(c.genome) && boost::filesystem::is_regular_file(c.genome) && boost::filesystem::file_size(c.genome)) {
    if (!svs.empty()) {
      if (!findPutativeSplitReads(c, svs, svType)) {
	std::cerr << "No split-read candidates found." << std::endl;
	std::cerr << "Please verify that the chromosome names between your reference sequence and bam file match." << std::endl;
	return -1;
      }
    } else {
      std::cerr << "No deletions found!" << std::endl;
      return -1;
    }
  }

  // VCF output
  if (svs.size()) {
    vcfOutput(c, svs, svType);
  }

  // End
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] Done." << std::endl;;
  return 0;
}


int main(int argc, char **argv) {
  Config c;

  // Define generic options
  boost::program_options::options_description generic("Generic options");
  generic.add_options()
    ("help,?", "show help message")
    ("type,t", boost::program_options::value<std::string>(&c.svType)->default_value("DEL"), "SV analysis type (DEL, DUP)")
    ("outfile,o", boost::program_options::value<boost::filesystem::path>(&c.outfile)->default_value("sv.vcf"), "SV output file")
    ("exclude,x", boost::program_options::value<boost::filesystem::path>(&c.exclude)->default_value(""), "file with chr to exclude")
    ;

  boost::program_options::options_description pem("PE options");
  pem.add_options()
    ("map-qual,q", boost::program_options::value<unsigned short>(&c.minMapQual)->default_value(0), "min. paired-end mapping quality")
    ("mad-cutoff,s", boost::program_options::value<unsigned short>(&c.madCutoff)->default_value(5), "insert size cutoff, median+s*MAD (deletions only)")
    ("variants,v", boost::program_options::value<boost::filesystem::path>(&c.variants), "BED interval file (turns PE off)")
    ;

  boost::program_options::options_description breaks("SR breakpoint options");
  breaks.add_options()
    ("genome,g", boost::program_options::value<boost::filesystem::path>(&c.genome), "genome fasta file")
    ("min-flank,m", boost::program_options::value<unsigned int>(&c.minimumFlankSize)->default_value(13), "minimum flanking sequence size")
    ("epsilon,e", boost::program_options::value<float>(&c.epsilon)->default_value(0.1), "allowed epsilon deviation of PE vs. SR deletion")
    ;

  // Define hidden options
  boost::program_options::options_description hidden("Hidden options");
  hidden.add_options()
    ("input-file", boost::program_options::value< std::vector<boost::filesystem::path> >(&c.files), "input file")
    ("pe-fraction,c", boost::program_options::value<float>(&c.percentAbnormal)->default_value(0.0), "fixed fraction c of discordant PEs, for c=0 MAD cutoff is used")
    ("num-split,n", boost::program_options::value<unsigned int>(&c.minimumSplitRead)->default_value(2), "minimum number of splitted reads")
    ("flanking,f", boost::program_options::value<unsigned int>(&c.flankQuality)->default_value(80), "quality of the aligned flanking region")
    ("pruning,j", boost::program_options::value<unsigned int>(&c.graphPruning)->default_value(100), "PE graph pruning cutoff")
    ("warranty,w", "show warranty")
    ("license,l", "show license")
    ;

  boost::program_options::positional_options_description pos_args;
  pos_args.add("input-file", -1);

  // Set the visibility
  boost::program_options::options_description cmdline_options;
  cmdline_options.add(generic).add(pem).add(breaks).add(hidden);
  boost::program_options::options_description visible_options;
  visible_options.add(generic).add(pem).add(breaks);
  boost::program_options::variables_map vm;
  boost::program_options::store(boost::program_options::command_line_parser(argc, argv).options(cmdline_options).positional(pos_args).run(), vm);
  boost::program_options::notify(vm);


  // Check command line arguments
  if ((vm.count("help")) || (!vm.count("input-file"))) { 
    printTitle("Deletion finder");
    if (vm.count("warranty")) {
      displayWarranty();
    } else if (vm.count("license")) {
      gplV3();
    } else {
      std::cout << "Usage: " << argv[0] << " [OPTIONS] <sample1.bam> <sample2.bam> ..." << std::endl;
      std::cout << visible_options << "\n"; 
    }
    return 1; 
  }

  // Show cmd
  boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
  std::cout << '[' << boost::posix_time::to_simple_string(now) << "] ";
  for(int i=0; i<argc; ++i) { std::cout << argv[i] << ' '; }
  std::cout << std::endl;

  // Do we need to run PE mapping
  if (boost::filesystem::exists(c.variants) && boost::filesystem::is_regular_file(c.variants) && boost::filesystem::file_size(c.variants)) c.peMode=false;
  else c.peMode=true;

  // Run main program
  if (c.svType == "DEL") return run(c, SVType<DeletionTag>());
  else if (c.svType == "DUP") return run(c, SVType<DuplicationTag>());
  else {
    std::cerr << "SV analysis type not supported by Delly: " << c.svType << std::endl;
    return -1;
  }
}
