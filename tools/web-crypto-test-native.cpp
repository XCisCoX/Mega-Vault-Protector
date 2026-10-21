// Headless native/browser interoperability fixture, never a user vault.
#include "videovault/core/vault.hpp"
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>
using namespace videovault::core;
int main(int argc,char** argv) {
  if(argc!=3) return 2;
  const std::filesystem::path root=argv[2];
  std::filesystem::create_directories(root);
  auto error=[](const VaultError& e){ std::cerr<<e.technical_detail<<'\n'; return 1; };
  const std::string password="test-password";
  if(std::string(argv[1])=="export") {
    auto v=Vault::create(root/"sender","local-fixture-only",{8192,1,1});
    if(!v) return error(v.error());
    std::vector<unsigned char> payload((1U<<20)+3,42);
    auto a=v.value().import_bytes(payload,"sample.bin");
    std::vector<unsigned char> empty;
    auto b=v.value().import_bytes(empty,"empty.bin");
    if(!a) return error(a.error()); if(!b) return error(b.error());
    for(bool compressed : {false,true}) {
      auto out=v.value().export_package(root/(compressed?"compressed.mvpk":"plain.mvpk"),password,
        {{a.value(),"folder/sample.bin"},{b.value(),"empty.bin"}},{"folder"},compressed);
      if(!out) return error(out.error());
    }
    std::cout<<"Native export: compressed and plain, two files, 1048579 payload bytes\n";
  } else if(std::string(argv[1])=="import") {
    auto v=Vault::create(root/"receiver","different-local-fixture",{8192,1,1});
    if(!v) return error(v.error());
    auto imported=v.value().import_package(root/"browser.mvpk",password,0);
    if(!imported) return error(imported.error());
    auto rows=v.value().list_videos();
    if(!rows) return error(rows.error());
    if(imported.value()!=1 || rows.value().size()!=1 || rows.value()[0].display_name!="sample.bin") return 3;
    auto bytes=v.value().read_video_bytes(rows.value()[0].id);
    if(!bytes) return error(bytes.error());
    if(bytes.value()!=std::vector<unsigned char>((1U<<20)+3,42)) return 4;
    std::cout<<"Native import: browser package matches all 1048579 bytes\n";
  } else return 2;
  return 0;
}
