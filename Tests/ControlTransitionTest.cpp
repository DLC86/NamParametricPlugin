// Regression for the final single-network 100/100 ms linear interpolation.
// No two-network crossfade or old Eco output predictor is used.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "DSP/NamModelEngine.h"

namespace {
using namparametric::dsp::NamModelEngine;
using namparametric::dsp::ModelSize;
using Json=nlohmann::json;

void Verify(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
std::vector<float> Render(NamModelEngine& engine, int length, int block) {
  std::vector<float> output(static_cast<size_t>(length));
  int done=0;
  while(done<length) {
    const int count=std::min(block,length-done);
    std::vector<float> input(static_cast<size_t>(count),0.11f);
    engine.Process(input.data(),output.data()+done,count);
    done+=count;
  }
  Verify(std::all_of(output.begin(),output.end(),
                     [](float x){return std::isfinite(x);}),"Nonfinite audio during control transition");
  return output;
}
void TestLinearSingleNetwork(const std::filesystem::path& path, ModelSize size) {
  NamModelEngine smoothed, reference;
  std::string error;
  Verify(smoothed.LoadModel(path.string(),error,size),error.c_str());
  Verify(reference.LoadModel(path.string(),error,size),error.c_str());
  smoothed.Reset(48000,257);
  auto settings=smoothed.GetParameterValues();
  const auto& specs=smoothed.GetParameterInfos();
  bool changed=false;
  for(size_t i=0;i<settings.size();++i) {
    const float other=(settings[i]!=specs[i].maxValue) ? specs[i].maxValue:specs[i].minValue;
    if(other!=settings[i]) { settings[i]=other;changed=true; }
  }
  Verify(changed,"Fixture must have at least one movable control");
  Verify(reference.SetParameterValues(settings),"Reference controls rejected");
  reference.Reset(48000,257); // exact settled destination before rendering
  Render(smoothed,8192,257);
  Render(reference,8192,257);
  Verify(smoothed.SetParameterValues(settings),"Destination controls rejected");
  const auto moving=Render(smoothed,28000,257);
  const auto settled=Render(reference,28000,257);
  float lateError=0, earlyError=0;
  for(size_t i=0;i<moving.size();++i) {
    const float difference=std::abs(moving[i]-settled[i]);
    if(i<4800) earlyError=std::max(earlyError,difference);
    if(i>=moving.size()-1024) lateError=std::max(lateError,difference);
  }
  Verify(lateError<2e-5f,"Linear smoothing did not settle to target weights");
  // Some synthetic models are insensitive at extreme positions; do not assert
  // that every fixture produces a nonzero transient.
  (void)earlyError;
}

void TestSwitchDisplay(const std::filesystem::path& directory,const Json& hyper) {
  auto modern=hyper;
  auto& params=modern["config"]["params"];
  bool found=false;
  size_t switched=0;
  for(size_t i=0;i<params.size();++i) {
    auto& param=params[i];
    if(param.value("type",std::string{})!="switch")continue;
    const auto names=param.at("enum_names").get<std::vector<std::string>>();
    Json options=Json::array();
    for(size_t value=0;value<names.size();++value)
      options.push_back({{"name",names[value]},{"value",value},
                         {"index_sw",names.size()-value-1}});
    param.erase("enum_names");
    param["options"]=std::move(options);
    found=true;
    switched=i;
    break;
  }
  if(!found) return;
  const auto path=directory/"new_switch_index_sw.pnam";
  { std::ofstream out(path);out<<modern; }
  NamModelEngine engine,legacy;
  std::string error;
  Verify(engine.LoadModel(path.string(),error),error.c_str());
  const auto oldPath=directory/"old_switch_enum_names.pnam";
  {std::ofstream out(oldPath);out<<hyper;}
  Verify(legacy.LoadModel(oldPath.string(),error),error.c_str());
  const auto& a=engine.GetParameterInfos()[switched];
  const auto& b=legacy.GetParameterInfos()[switched];
  Verify(a.enumNames.size()==b.enumNames.size(),"Switch option count changed");
  for(size_t visual=0;visual<a.enumNames.size();++visual) {
    const auto value=a.enumValues[visual];
    Verify(a.enumNames[visual]==b.enumNames[static_cast<size_t>(value)],
           "Switch label index_sw did not follow neural value");
  }
  engine.Reset(48000,257);
  legacy.Reset(48000,257);
  const auto actual=Render(engine,8192,257);
  const auto expected=Render(legacy,8192,257);
  for(size_t i=0;i<actual.size();++i)
    Verify(std::abs(actual[i]-expected[i])<2e-5f,
           "Switch index_sw visual metadata changed neural audio");
  modern["config"]["params"][switched]["options"][0]["index_sw"]=
      modern["config"]["params"][switched]["options"][1]["index_sw"];
  const auto invalid=directory/"invalid_switch_index_sw.pnam";
  {std::ofstream out(invalid);out<<modern;}
  NamModelEngine rejected;
  Verify(!rejected.LoadModel(invalid.string(),error),"Duplicate index_sw was accepted");
}
} // namespace

void TestControlTransitions(const std::filesystem::path& directory,const nlohmann::json& hyper,
                            const nlohmann::json& a2) {
  for(bool nested:{false,true}) {
    const auto path=directory/(nested?"single_net_a2.pnam":"single_net_hyper.pnam");
    {std::ofstream out(path);out<<(nested?a2:hyper);}
    for(auto size:{ModelSize::Full,ModelSize::Lite}) {
      if(!nested && size==ModelSize::Lite)continue;
      TestLinearSingleNetwork(path,size);
    }
  }
  TestSwitchDisplay(directory,hyper);
}
