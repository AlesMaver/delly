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

#ifndef JSON_H
#define JSON_H

#include <iostream>
#include <fstream>
#include <boost/property_tree/json_parser.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/iostreams/stream_buffer.hpp>
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <boost/iostreams/filter/zlib.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/filesystem.hpp>
#include <boost/progress.hpp>


namespace torali
{

  inline void
    printPt(boost::property_tree::ptree const& pt) {
    for(boost::property_tree::ptree::const_iterator it = pt.begin(); it!=pt.end(); ++it) {
      std::cout << it->first << ": " << it->second.get_value<std::string>() << std::endl;
      printPt(it->second);
    }
  }
      

  // Parse json file
  template<typename TConfig, typename TStructuralVariantRecord, typename TTag>
  inline void
  jsonParse(TConfig const& c, bam_hdr_t* hd, std::vector<TStructuralVariantRecord>& svs, SVType<TTag> svType) {
    std::ifstream file(c.vcffile.string().c_str(), std::ios_base::in | std::ios_base::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> dataIn;
    dataIn.push(boost::iostreams::gzip_decompressor());
    dataIn.push(file);
    std::istream instream(&dataIn);
    std::string line;
    while(std::getline(instream, line)) {
      boost::property_tree::ptree pt;
      std::stringstream iss(line);
      boost::property_tree::read_json(iss, pt);
      if (pt.get<std::string>("info.svtype", "None") != _addID(svType)) continue;      

      // Fill SV record
      StructuralVariantRecord svRec;
      std::string chrName(pt.get<std::string>("chrom", "None"));
      int32_t tid = bam_name2id(hd, chrName.c_str());
      svRec.chr = tid;
      svRec.svStart = pt.get<int32_t>("start", 0) + 1;
      svRec.svEnd = pt.get<int32_t>("info.end", 0);
      svRec.id = pt.get<int32_t>("id", 0);
      if (pt.get<bool>("info.precise", false)) svRec.precise = true;
      else svRec.precise = false;
      svRec.peSupport = pt.get<int32_t>("info.pe", 0);
      svRec.insLen = pt.get<int32_t>("info.inslen", 0);
      svRec.homLen = pt.get<int32_t>("info.homlen", 0);
      svRec.srSupport = pt.get<int32_t>("info.sr", 0);
      svRec.consensus = pt.get<std::string>("info.consensus", "");
      boost::property_tree::ptree::const_iterator it = pt.get_child("info.cipos.").begin();
      svRec.wiggle = -1 * it->second.get_value<int32_t>();
      svRec.peMapQuality = pt.get<uint8_t>("info.mapq", 0);
      svRec.srAlignQuality = pt.get<double>("info.srq", 0);
      svRec.svt = -1;
      std::string chr2Name(pt.get<std::string>("info.chr2", "None"));
      svRec.chr2 = bam_name2id(hd, chr2Name.c_str());
      svs.push_back(svRec);
    }

    // Close JSON file
    file.close();
  }

  template<typename TConfig, typename TStructuralVariantRecord, typename TJunctionCountMap, typename TReadCountMap, typename TCountMap, typename TTag>
    inline void
    jsonOutput(TConfig const& c, std::vector<TStructuralVariantRecord> const& svs, TJunctionCountMap const& jctCountMap, TReadCountMap const& readCountMap, TCountMap const& spanCountMap, SVType<TTag> svType) 
  {
    // BoLog class
    BoLog<double> bl;

    boost::posix_time::ptime now = boost::posix_time::second_clock::local_time();
    std::cout << '[' << boost::posix_time::to_simple_string(now) << "] " << "Genotyping" << std::endl;
    boost::progress_display show_progress( svs.size() );

    // Open one bam file header
    samFile* samfile = sam_open(c.files[0].string().c_str(), "r");
    bam_hdr_t* bamhd = sam_hdr_read(samfile);
    
    // Output all structural variants
    boost::iostreams::filtering_ostream dataOut;
    dataOut.push(boost::iostreams::gzip_compressor());
    dataOut.push(boost::iostreams::file_sink(c.outfile.string().c_str(), std::ios_base::out | std::ios_base::binary));
    
    // Genotype arrays
    int32_t *gts = (int*) malloc(c.files.size() * 2 * sizeof(int));
    float *gls = (float*) malloc(c.files.size() * 3 * sizeof(float));
    int32_t *rcl = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *rc = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *rcr = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *cnest = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *drcount = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *dvcount = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *rrcount = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *rvcount = (int*) malloc(c.files.size() * sizeof(int));
    int32_t *gqval = (int*) malloc(c.files.size() * sizeof(int));
    std::vector<std::string> ftarr;
    ftarr.resize(c.files.size());

    // Iterate all structural variants
    typedef std::vector<TStructuralVariantRecord> TSVs;
    uint32_t lastId = svs.size();
    for(typename TSVs::const_iterator svIter = svs.begin(); svIter!=svs.end(); ++svIter) {
      ++show_progress;
      
      // Set values
      std::string filterVal = "PASS";
      if ((svIter->precise) && (svIter->chr == svIter->chr2)) {
	if ((svIter->srSupport < 3) || (svIter->peMapQuality < 20)) filterVal = "LowQual";
      } else {
	if ((svIter->peSupport < 3) || (svIter->peMapQuality < 20) || ( (svIter->chr != svIter->chr2) && (svIter->peSupport < 5) ) ) filterVal = "LowQual";
      }
      std::string preciseVal = "false";
      if (svIter->precise) preciseVal = "true";
      std::string dellyVersion("EMBL.DELLYv");
      dellyVersion += dellyVersionNumber;

      // Output main fields
      dataOut << '{';
      dataOut << "\"chrom\": \"" << bamhd->target_name[svIter->chr] << "\", ";
      dataOut << "\"start\": " << svIter->svStart - 1 << ", ";
      if (_addID(svType) != "BND") dataOut << "\"end\": " << svIter->svEnd << ", ";
      else dataOut << "\"end\": " << svIter->svStart << ", ";
      dataOut << "\"id\": " << svIter->id << ", ";
      if (_addID(svType) == "BND") {
	dataOut << "\"$children\": [{";
	dataOut << "\"chrom\": \"" << bamhd->target_name[svIter->chr2] << "\", ";
	dataOut << "\"start\": " << svIter->svEnd << ", ";
	dataOut << "\"end\": " << svIter->svEnd + 1 << ", ";
	dataOut << "\"id\": " << (svIter->id + svs.size()) << "";
	dataOut << "}] ,";
      }
      dataOut << "\"ref\": \"N\", \"alt\": \"" << _addID(svType) << "\", ";
      dataOut << "\"qual\": 0, ";
      dataOut << "\"filter\": \"" << filterVal << "\", ";
      dataOut << "\"info\": {";
      dataOut << "\"precise\": " << preciseVal << ", ";
      dataOut << "\"svtype\": \"" << _addID(svType) << "\", ";
      dataOut << "\"svmethod\": \"" << dellyVersion << "\", ";
      dataOut << "\"chr2\": \"" << bamhd->target_name[svIter->chr2] << "\", ";
      dataOut << "\"end\": \"" << svIter->svEnd << "\", ";
      dataOut << "\"inslen\": " << svIter->insLen << ", ";
      dataOut << "\"homlen\": " << svIter->homLen << ", ";
      dataOut << "\"pe\": " << svIter->peSupport << ", ";
      dataOut << "\"mapq\": " << (int) (svIter->peMapQuality) << ", ";
      dataOut << "\"cipos\": [" << -svIter->wiggle << ", " << svIter->wiggle << "], ";
      dataOut << "\"ciend\": [" << -svIter->wiggle << ", " << svIter->wiggle << "], ";
      dataOut << "\"ct\": \"" << _addOrientation(svIter->ct) << "\"";
      if (svIter->precise)  {
	dataOut << ", ";
	dataOut << "\"sr\": " << svIter->srSupport << ", ";
	dataOut << "\"srq\": " << (float) (svIter->srAlignQuality) << ", ";
	dataOut << "\"consensus\": \"" << svIter->consensus << "\", ";
	dataOut << "\"ce\": " << (float) (entropy(svIter->consensus));
      }
      dataOut << "}, ";


      // Add genotype columns
      for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
	// Counters
	rcl[file_c] = 0;
	rc[file_c] = 0;
	rcr[file_c] = 0;
	cnest[file_c] = 0;
	drcount[file_c] = 0;
	dvcount[file_c] = 0;
	rrcount[file_c] = 0;
	rvcount[file_c] = 0;

	if (spanCountMap[file_c][svIter->id].ref.size() < spanCountMap[file_c][lastId + svIter->id].ref.size()) {
	  drcount[file_c] = spanCountMap[file_c][svIter->id].ref.size();
	  dvcount[file_c] = spanCountMap[file_c][svIter->id].alt.size();
	} else {
	  drcount[file_c] = spanCountMap[file_c][lastId + svIter->id].ref.size();
	  dvcount[file_c] = spanCountMap[file_c][lastId + svIter->id].alt.size();
	}
	rrcount[file_c] = jctCountMap[file_c][svIter->id].ref.size();
	rvcount[file_c] = jctCountMap[file_c][svIter->id].alt.size();
	
	// Compute GLs
	if (svIter->precise) _computeGLs(bl, jctCountMap[file_c][svIter->id].ref, jctCountMap[file_c][svIter->id].alt, gls, gqval, gts, file_c);
	else {  // Imprecise SVs
	  if (spanCountMap[file_c][svIter->id].ref.size() < spanCountMap[file_c][lastId + svIter->id].ref.size())
	    _computeGLs(bl, spanCountMap[file_c][svIter->id].ref, spanCountMap[file_c][svIter->id].alt, gls, gqval, gts, file_c);
	  else
	    _computeGLs(bl, spanCountMap[file_c][lastId + svIter->id].ref, spanCountMap[file_c][lastId + svIter->id].alt, gls, gqval, gts, file_c);
	}
	
	// Compute RCs
	rcl[file_c] = readCountMap[file_c][svIter->id].leftRC;
	rc[file_c] = readCountMap[file_c][svIter->id].rc;
	rcr[file_c] = readCountMap[file_c][svIter->id].rightRC;
	cnest[file_c] = -1;
	if ((rcl[file_c] + rcr[file_c]) > 0) cnest[file_c] = boost::math::iround( 2.0 * (double) rc[file_c] / (double) (rcl[file_c] + rcr[file_c]) );
      
	// Genotype filter
	if (gqval[file_c] < 15) ftarr[file_c] = "LowQual";
	else ftarr[file_c] = "PASS";
      }

      // Output genotypes
      dataOut << "\"genotypes\": {";
      for(unsigned int file_c = 0; file_c < c.files.size(); ++file_c) {
	dataOut << "\"" << c.sampleName[file_c] << "\": {";
	dataOut << "\"gt\": [" << bcf_gt_allele(gts[file_c*2]) << ", " << bcf_gt_allele(gts[file_c*2+1]) << "], ";
	dataOut << "\"gl\": [" << gls[file_c*3] << ", " << gls[file_c*3+1] << ", " << gls[file_c*3+2] << "], ";
	dataOut << "\"gq\": " << gqval[file_c] << ", ";
	dataOut << "\"ft\": \"" << ftarr[file_c] << "\", ";
	dataOut << "\"rcl\": " << rcl[file_c] << ", ";
	dataOut << "\"rc\": " << rc[file_c] << ", ";
	dataOut << "\"rcr\": " << rcr[file_c] << ", ";
	dataOut << "\"cn\": " << cnest[file_c] << ", ";
	dataOut << "\"dr\": " << drcount[file_c] << ", ";
	dataOut << "\"dv\": " << dvcount[file_c] << ", ";
	dataOut << "\"rr\": " << rrcount[file_c] << ", ";
	dataOut << "\"rv\": " << rvcount[file_c] << "";
	dataOut << "}";
      }
      dataOut << "}";
      dataOut << '}' << std::endl;

      // Add children
      if (_addID(svType) == "BND") {
	dataOut << '{';
	dataOut << "\"chrom\": \"" << bamhd->target_name[svIter->chr2] << "\", ";
	dataOut << "\"start\": " << svIter->svEnd << ", ";
	dataOut << "\"end\": " << svIter->svEnd + 1 << ", ";
	dataOut << "\"id\": " << (svIter->id + svs.size()) << ", ";
	dataOut << "\"$parent\": {";
	dataOut << "\"chrom\": \"" << bamhd->target_name[svIter->chr] << "\", ";
	dataOut << "\"start\": " << svIter->svStart - 1 << ", ";
	dataOut << "\"end\": " << svIter->svStart << ", ";
	dataOut << "\"id\": " << svIter->id << "";
	dataOut << "}";
	dataOut << '}' << std::endl;
      }
    }
    
    // Clean-up
    free(gts);
    free(gls);
    free(rcl);
    free(rc);
    free(rcr);
    free(cnest);
    free(drcount);
    free(dvcount);
    free(rrcount);
    free(rvcount);
    free(gqval);
    
    // Close BAM file
    bam_hdr_destroy(bamhd);
    sam_close(samfile);
  }  

}

#endif
