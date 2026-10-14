//-*-C++-*-

#ifndef PDF_PAGE_FONT_CIDS_H
#define PDF_PAGE_FONT_CIDS_H

#include <fstream>

#include <mutex>
#include <unordered_map>

namespace pdflib
{

  class font_cids
  {

  public:

    font_cids();
    ~font_cids();

    void initialise(std::string dirname);

    bool      has(std::string font_name);
    font_cid& get(std::string encoding_name);

    int get_supplement(std::string registry, 
		       std::string ordering);

    bool decode_cmap_resource(std::string font_name);

    // For Identity-H / Identity-V CID fonts: look up the CIDSystemInfo
    // (registry + ordering, e.g. "Adobe" + "CNS1") and fill `out_map` with
    // the full CID → UTF-8 table from the bundled cid2code.txt.
    // Entries already present in `out_map` (from /ToUnicode) are NOT replaced.
    // Returns true when the Registry-Ordering is known, false otherwise.
    bool fill_identity_cmap(const std::string& registry,
                             const std::string& ordering,
                             std::unordered_map<uint32_t, std::string>& out_map);

    void decode_all();

  private:

    bool initialized;
    std::string directory;

    // `cids` is filled lazily, on first use of a cmap, while the threaded
    // parser decodes pages on several workers at once. Guard every access to
    // it: unsynchronised inserts are a data race, and a second thread must not
    // observe a half-built font_cid -- it would resolve only part of the
    // codes and silently produce different text for the same page.
    std::mutex cids_mutex;

    std::unordered_map<std::string, int>                       ro_2_sup;
    std::unordered_map<std::string, std::vector<std::string> > ros_2_cols;

    // Registry-Ordering key (e.g. "Adobe-CNS1") → {cid2code path, columns}
    // Built during initialise() for every ROS we ship resources for.
    std::unordered_map<std::string,
        std::pair<std::string, std::vector<std::string>>> ro_2_resource;

    std::unordered_map<std::string, std::string> cmap_2_filename;
    std::unordered_map<std::string, std::string> cmap_2_cid2code;
    std::unordered_map<std::string, std::vector<std::string> > cmap_2_columns;

    std::unordered_map<std::string, font_cid> cids;
  };

  font_cids::font_cids():
    initialized(false)
  {}

  font_cids::~font_cids()
  {}

  bool font_cids::has(std::string font_name)
  {
    //LOG_S(INFO) << __FUNCTION__ << ": " << font_name << "\t" 
    //<< cmap_2_filename.count(font_name);

    return (cmap_2_filename.count(font_name)==1);
  }

  font_cid& font_cids::get(std::string encoding_name)
  {
    std::lock_guard<std::mutex> guard(cids_mutex);

    // Safe to hand out past the lock: unordered_map keeps references to its
    // elements valid across rehashing, and a decoded font_cid is read-only.
    return cids[encoding_name];
  }

  int font_cids::get_supplement(std::string registry, 
				std::string ordering)
  {
    LOG_S(INFO) << __FUNCTION__;

    std::string key = registry+"-"+ordering;

    for(auto itr=ro_2_sup.begin(); itr!=ro_2_sup.end(); itr++)
      {
	if(itr->first==key)
	  {
	    return itr->second;
	  }
      }

    return -1;
  }

  void font_cids::initialise(std::string dirname)
  {
    if(initialized)
      {
	LOG_S(WARNING) << "skipping font_cids::initialise, already initialized ...";
	return;
      }
    
    LOG_S(INFO) << "initialise font_cids";

    directory = dirname;
    directory += (directory.back()=='/'? "" : "/");

    ro_2_sup = {
      { "Adobe-CNS1"  , 7},
      { "Adobe-KR"    , 9},
      { "Adobe-Japan1", 7},
      { "Adobe-GB1"   , 5},
      { "Adobe-Korea1", 2}
    };

    ros_2_cols = {
      { "Adobe-CNS1-7"  , {"UniCNS-UTF16"} },
      { "Adobe-KR-9"    , {"UniAKR-UTF16"} },
      { "Adobe-Japan1-7", {"UniJIS-UTF16", "Roman"} },
      { "Adobe-GB1-5"   , {"UniGB-UTF16"} },
      { "Adobe-Korea1-2", {"UniKS-UTF16"} }
    };

    for(auto itr=ros_2_cols.begin(); itr!=ros_2_cols.end(); itr++)
      {
	std::string cdir = directory+(itr->first)+"/CMap";
	
	std::vector<std::string> files = utils::filesystem::list_files(cdir, true);

	for(auto file:files)
	  {
	    std::string key = "/"+file;
	    LOG_S(INFO) << __FUNCTION__ << "\t" << file;
	    
	    cmap_2_columns[key]  = (itr->second);
	    cmap_2_columns[file] = (itr->second);

	    cmap_2_cid2code[key]  = directory+(itr->first)+"/cid2code.txt";
	    cmap_2_cid2code[file] = directory+(itr->first)+"/cid2code.txt";
	    
	    cmap_2_filename[key]  = cdir+"/"+file;
	    cmap_2_filename[file] = cdir+"/"+file;
	  }
      }

    // Build the Registry-Ordering → resource lookup used by fill_identity_cmap().
    for(auto itr=ros_2_cols.begin(); itr!=ros_2_cols.end(); itr++)
      {
        // itr->first is e.g. "Adobe-CNS1-7"; strip the trailing "-<supplement>"
        // to get the RO key "Adobe-CNS1".
        size_t last_dash = itr->first.rfind('-');
        if(last_dash == std::string::npos) { continue; }

        std::string ro_key   = itr->first.substr(0, last_dash);
        std::string cid2code = directory+(itr->first)+"/cid2code.txt";

        // Only record the first (highest-versioned) entry encountered; the map
        // is built from ros_2_cols which lists each ROS once.
        if(ro_2_resource.count(ro_key) == 0)
          {
            ro_2_resource[ro_key] = {cid2code, itr->second};
            LOG_S(INFO) << __FUNCTION__ << ": registered Identity fallback for "
                        << ro_key << " -> " << cid2code;
          }
      }

    initialized = true;
  }

  bool font_cids::fill_identity_cmap(const std::string& registry,
                                      const std::string& ordering,
                                      std::unordered_map<uint32_t, std::string>& out_map)
  {
    LOG_S(INFO) << __FUNCTION__ << ": " << registry << "-" << ordering;

    std::string ro_key = registry + "-" + ordering;

    auto res_it = ro_2_resource.find(ro_key);
    if(res_it == ro_2_resource.end())
      {
        LOG_S(WARNING) << "fill_identity_cmap: unknown ROS " << ro_key;
        return false;
      }

    const std::string&              cid2code_path = res_it->second.first;
    const std::vector<std::string>& columns       = res_it->second.second;

    // Use a dedicated cache key so the Identity-mode font_cid stays separate
    // from any CMap-file-based font_cid for the same ROS.
    std::string cache_key = ro_key + "-identity";

    std::lock_guard<std::mutex> guard(cids_mutex);

    if(cids.count(cache_key) == 0)
      {
        font_cid& cid = cids[cache_key];
        cid.decode_identity_with_cid2code(cid2code_path, columns);
      }

    for(const auto& entry : cids[cache_key].get())
      {
        out_map.emplace(entry.first, entry.second);  // don't overwrite existing
      }

    LOG_S(INFO) << "fill_identity_cmap: out_map now has " << out_map.size() << " entries";
    return true;
  }

  bool font_cids::decode_cmap_resource(std::string cmap_name)
  {
    LOG_S(INFO) << __FUNCTION__;    

    // Held across the whole decode, so a concurrent caller either waits and
    // then sees the finished entry, or does not see the entry at all.
    std::lock_guard<std::mutex> guard(cids_mutex);

    // fetch pre-cached cid-fonts
    if(cids.count(cmap_name)==1)
      {
	//cid = cids[cmap_name];
	return true;
      }
    else if(cmap_2_cid2code.count(cmap_name)==1)
      {
	std::vector<std::string> columns = cmap_2_columns[cmap_name];
	std::string cid2code = cmap_2_cid2code[cmap_name];
	std::string filename = cmap_2_filename[cmap_name];
	
	font_cid& cid = cids[cmap_name];
	cid.decode_cmap_resource(filename, cid2code, columns);

	//cids[cmap_name] = cid;
	return true;
      }
    else
      {
	LOG_S(INFO) << "unknown cmap-name: " << cmap_name;    	
	return false;
      }
  }

  void font_cids::decode_all()
  {
    LOG_S(INFO) << __FUNCTION__;

    for(auto itr=cmap_2_filename.begin(); itr!=cmap_2_filename.end(); itr++)
      {
	std::string cmap_name = itr->first; 

	if(cmap_name.front()=='/')
	  {
	    font_cid cid;
	    
	    std::vector<std::string> columns = cmap_2_columns[cmap_name];
	    std::string cid2code = cmap_2_cid2code[cmap_name];
	    std::string filename = cmap_2_filename[cmap_name];
	    
	    cid.decode_cmap_resource(filename, cid2code, columns);
	  }
      }
  }

}

#endif
