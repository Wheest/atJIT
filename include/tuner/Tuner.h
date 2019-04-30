#pragma once

#include <tuner/Feedback.h>
#include <tuner/KnobConfig.h>
#include <tuner/KnobSet.h>

#include <llvm/IR/Module.h>

#include <algorithm>
#include <iostream>

namespace tuner {

  using GenResult = std::pair<std::shared_ptr<KnobConfig>, std::shared_ptr<Feedback>>;

  // generic operations over KnobSet/KnobConfig components.
  namespace {
    class KnobSetter : public KnobConfigAppFn {
      KnobSet &KS;

    public:
      KnobSetter(KnobSet &KSet) : KS(KSet) {}

      #define HANDLE_CASE(ValTy, KnobMap)                                      \
        void operator()(std::pair<KnobID, ValTy> Entry) override {             \
          auto ID = Entry.first;                                               \
          auto NewVal = Entry.second;                                          \
          auto Knobs = KS.KnobMap;                                             \
          auto TheKnob = Knobs[ID];                                            \
          TheKnob->setVal(NewVal);                                             \
        }

      HANDLE_CASE(int, IntKnobs)
      HANDLE_CASE(LoopSetting, LoopKnobs)

      #undef HANDLE_CASE

    }; // end class


    class KnobApplier : public KnobSetAppFn {
      llvm::Module &M;

    public:
      KnobApplier(llvm::Module &M_) : M(M_) {}

      #define HANDLE_CASE(KnobKind)                                            \
        void operator()(std::pair<KnobID, KnobKind> Entry) override {          \
          auto TheKnob = Entry.second;                                         \
          TheKnob->apply(M);                                                   \
        }

      HANDLE_CASE(knob_type::ScalarInt*)
      HANDLE_CASE(knob_type::Loop*)

      #undef HANDLE_CASE

    }; // end class

  } // end anonymous namespace

  struct {
    bool operator()(GenResult const &A, GenResult const &B) const
    {  // A >= B if B is better than A
        return B.second->betterThan(*A.second);
    }
  } resultGEQ;

  struct {
    bool operator()(GenResult const &A, GenResult const &B) const
    {  // A < B if A is better than B
        return A.second->betterThan(*B.second);
    }
  } resultLT;

  class Tuner {
  private:
    std::mutex configLock;
    std::vector<GenResult> Configs_;

  protected:
    KnobSet KS_;

  public:

    Tuner(KnobSet KS) : KS_(KS) {}

    void addConfig(GenResult R) {
      std::lock_guard<std::mutex> guard(configLock);
      getConfigs().push_back(R);
    }

    // must be holding the lock
    std::vector<GenResult>& getConfigs() {
      return Configs_;
    }
    std::mutex& getConfigLock() { return configLock; }

    // ensures derived destructors are called
    virtual ~Tuner() = default;

    // this method should not mutate the value of any Knobs,
    // it _may_ mutate the state of the Tuner.
    // NOTE: NOT THREAD SAFE.
    virtual GenResult getNextConfig () = 0;

    // a method to query whether the tuner
    // should produce a new config, given no
    // additional measurement feedback information
    // from prior configs. This is used to allow more
    // compilation tasks to occur concurrently.
    // NOTE: NOT THREAD SAFE
    virtual bool shouldCompileNext() = 0;

    virtual void analyze(llvm::Module &M) = 0;

    // applies a configuration to the given LLVM module via the
    // knobs managed by this tuner.
    void applyConfig (KnobConfig const &Config, llvm::Module &M) {
      KnobSetter ChangeKnobs(KS_);
      KnobApplier ModifyModule(M);

      // NOTE: we break this into two steps, making it
      // safe to assume that all knobs have been set to the
      // config when being applied. This is useful to know if
      // knobs contain pointers to other knobs, like LoopKnobs do.

      applyToConfig(ChangeKnobs, Config);
      applyToKnobs(ModifyModule, KS_);
    }

    std::optional<GenResult> bestSeen() {
      std::optional<GenResult> best = std::nullopt;

      std::lock_guard<std::mutex> guard(getConfigLock());
      auto Configs = getConfigs();
      for (auto Entry : Configs) {
        if (best.has_value() == false || resultGEQ(best.value(), Entry))
          best = Entry;
      }

      return best;
    }

    KnobSet const& getKnobSet() const {
      return KS_;
    }

    virtual void dump() {
      auto best = bestSeen();
      std::cerr << "\n---------- best config ----------\n";

      if (best.has_value())
        dumpConfigInstance(std::cerr, KS_, best.value());
      else
        std::cerr << "<no configs generated yet>\n\n";
    }

    void dumpStats(std::ostream &file) {
      // sort the configs before dumping
      std::lock_guard<std::mutex> guard(getConfigLock());
      auto Configs = getConfigs();

      std::sort(Configs.begin(), Configs.end(), resultLT);

      JSON::beginBind(file, "versions");
      JSON::beginArray(file);

      bool pastFirst = false;
      for (auto Entry : Configs) {
        if (pastFirst)
          JSON::comma(file);
        pastFirst |= true;

        dumpConfigInstance(file, KS_, Entry);
      }

      JSON::endArray(file);
    }

  }; // end class Tuner

  class NoOpTuner : public Tuner {
  private:
    GenResult NoOpConfig_;
  public:

    NoOpTuner(KnobSet KS) : Tuner(KS) {
      NoOpConfig_ = { std::make_shared<KnobConfig>(),
                      std::make_shared<NoOpFeedback>() };
    }

    GenResult getNextConfig () override {
      return NoOpConfig_;
    }

    bool shouldCompileNext () override {
      return false; // no need
    }

    void analyze(llvm::Module &M) override { }

    // dump nothing if using the noop tuner. This should
    // quiet down some noise when using standard JIT calls.
    void dump() override { }
  };

} // namespace tuner
