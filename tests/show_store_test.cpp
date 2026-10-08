#include <doctest/doctest.h>

#include <fstream>
#include <thread>

#include "core/show_store.h"
#include "support/temp_dir.h"

using namespace std::chrono_literals;

namespace {
bc::Show showNamed(const std::string& name) {
  bc::Show s;
  bc::CameraConfig cam;
  cam.id = "cam1";
  cam.name = name;
  cam.input.params = {{"mode", "listener"}, {"port", 9000}, {"passphrase", "supersecret123"}};
  s.cameras.push_back(cam);
  return s;
}
std::string readAll(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), {}};
}
}  // namespace

TEST_CASE("ShowStore: save then load returns the same show, secrets encrypted on disk") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path);
  store.saveNow(showNamed("Cam A"));
  CHECK(readAll(store.mainPath()).find("supersecret123") == std::string::npos);
  auto loaded = store.load();
  CHECK(loaded.from == bc::ShowStore::From::Main);
  CHECK(loaded.warning.empty());
  CHECK(loaded.show == showNamed("Cam A"));
}

TEST_CASE("ShowStore: a corrupt show.json falls back to last-known-good with a warning") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path);
  store.saveNow(showNamed("Cam A"));
  std::ofstream(store.mainPath(), std::ios::trunc) << "{not json";
  auto loaded = store.load();
  CHECK(loaded.from == bc::ShowStore::From::LastKnownGood);
  CHECK_FALSE(loaded.warning.empty());
  CHECK(loaded.show == showNamed("Cam A"));
}

TEST_CASE("ShowStore: nothing on disk gives the default show without a warning") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path);
  auto loaded = store.load();
  CHECK(loaded.from == bc::ShowStore::From::Default);
  CHECK(loaded.warning.empty());
  CHECK(loaded.show.cameras.empty());
  CHECK(loaded.show.settings.rate == bc::Rate{60, 1});
}

TEST_CASE("ShowStore: rapid changes are coalesced into one save") {
  bc::test::TempDir dir;
  bc::ShowStore store(dir.path, 50ms);
  store.scheduleSave(showNamed("one"));
  store.scheduleSave(showNamed("two"));
  store.scheduleSave(showNamed("three"));
  std::this_thread::sleep_for(400ms);
  CHECK(store.saveCount() == 1);
  CHECK(store.load().show.cameras[0].name == "three");
}

TEST_CASE("ShowStore: destruction flushes a pending save") {
  bc::test::TempDir dir;
  {
    bc::ShowStore store(dir.path, 10s);
    store.scheduleSave(showNamed("pending"));
  }
  bc::ShowStore again(dir.path);
  CHECK(again.load().show.cameras[0].name == "pending");
}
