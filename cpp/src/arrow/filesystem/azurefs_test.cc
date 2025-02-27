// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <algorithm>  // Missing include in boost/process

// This boost/asio/io_context.hpp include is needless for no MinGW
// build.
//
// This is for including boost/asio/detail/socket_types.hpp before any
// "#include <windows.h>". boost/asio/detail/socket_types.hpp doesn't
// work if windows.h is already included. boost/process.h ->
// boost/process/args.hpp -> boost/process/detail/basic_cmd.hpp
// includes windows.h. boost/process/args.hpp is included before
// boost/process/async.h that includes
// boost/asio/detail/socket_types.hpp implicitly is included.
#include <boost/asio/io_context.hpp>
// We need BOOST_USE_WINDOWS_H definition with MinGW when we use
// boost/process.hpp. See BOOST_USE_WINDOWS_H=1 in
// cpp/cmake_modules/ThirdpartyToolchain.cmake for details.
#include <boost/process.hpp>

#include "arrow/filesystem/azurefs.h"
#include "arrow/filesystem/azurefs_internal.h"

#include <random>
#include <string>

#include <gmock/gmock-matchers.h>
#include <gmock/gmock-more-matchers.h>
#include <gtest/gtest.h>
#include <azure/identity/client_secret_credential.hpp>
#include <azure/identity/default_azure_credential.hpp>
#include <azure/identity/managed_identity_credential.hpp>
#include <azure/storage/blobs.hpp>
#include <azure/storage/common/storage_credential.hpp>
#include <azure/storage/files/datalake.hpp>

#include "arrow/filesystem/path_util.h"
#include "arrow/filesystem/test_util.h"
#include "arrow/result.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/testing/util.h"
#include "arrow/util/io_util.h"
#include "arrow/util/key_value_metadata.h"
#include "arrow/util/logging.h"
#include "arrow/util/string.h"
#include "arrow/util/value_parsing.h"

namespace arrow {
using internal::TemporaryDir;
namespace fs {
namespace {
namespace bp = boost::process;

using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::NotNull;

namespace Blobs = Azure::Storage::Blobs;
namespace Files = Azure::Storage::Files;

auto const* kLoremIpsum = R"""(
Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor
incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis
nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat.
Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu
fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in
culpa qui officia deserunt mollit anim id est laborum.
)""";

class AzuriteEnv : public ::testing::Environment {
 public:
  AzuriteEnv() {
    account_name_ = "devstoreaccount1";
    account_key_ =
        "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/"
        "KBHBeksoGMGw==";
    auto exe_path = bp::search_path("azurite");
    if (exe_path.empty()) {
      auto error = std::string("Could not find Azurite emulator.");
      status_ = Status::Invalid(error);
      return;
    }
    auto temp_dir_ = *TemporaryDir::Make("azurefs-test-");
    auto debug_log_path_result = temp_dir_->path().Join("debug.log");
    if (!debug_log_path_result.ok()) {
      status_ = debug_log_path_result.status();
      return;
    }
    debug_log_path_ = *debug_log_path_result;
    server_process_ =
        bp::child(boost::this_process::environment(), exe_path, "--silent", "--location",
                  temp_dir_->path().ToString(), "--debug", debug_log_path_.ToString());
    if (!(server_process_.valid() && server_process_.running())) {
      auto error = "Could not start Azurite emulator.";
      server_process_.terminate();
      server_process_.wait();
      status_ = Status::Invalid(error);
      return;
    }
    status_ = Status::OK();
  }

  ~AzuriteEnv() override {
    server_process_.terminate();
    server_process_.wait();
  }

  Result<int64_t> GetDebugLogSize() {
    ARROW_ASSIGN_OR_RAISE(auto exists, arrow::internal::FileExists(debug_log_path_));
    if (!exists) {
      return 0;
    }
    ARROW_ASSIGN_OR_RAISE(auto file_descriptor,
                          arrow::internal::FileOpenReadable(debug_log_path_));
    ARROW_RETURN_NOT_OK(arrow::internal::FileSeek(file_descriptor.fd(), 0, SEEK_END));
    return arrow::internal::FileTell(file_descriptor.fd());
  }

  Status DumpDebugLog(int64_t position = 0) {
    ARROW_ASSIGN_OR_RAISE(auto exists, arrow::internal::FileExists(debug_log_path_));
    if (!exists) {
      return Status::OK();
    }
    ARROW_ASSIGN_OR_RAISE(auto file_descriptor,
                          arrow::internal::FileOpenReadable(debug_log_path_));
    if (position > 0) {
      ARROW_RETURN_NOT_OK(arrow::internal::FileSeek(file_descriptor.fd(), position));
    }
    std::vector<uint8_t> buffer;
    const int64_t buffer_size = 4096;
    buffer.reserve(buffer_size);
    while (true) {
      ARROW_ASSIGN_OR_RAISE(
          auto n_read_bytes,
          arrow::internal::FileRead(file_descriptor.fd(), buffer.data(), buffer_size));
      if (n_read_bytes <= 0) {
        break;
      }
      std::cerr << std::string_view(reinterpret_cast<const char*>(buffer.data()),
                                    n_read_bytes);
    }
    std::cerr << std::endl;
    return Status::OK();
  }

  const std::string& account_name() const { return account_name_; }
  const std::string& account_key() const { return account_key_; }
  const Status status() const { return status_; }

 private:
  std::string account_name_;
  std::string account_key_;
  bp::child server_process_;
  Status status_;
  std::unique_ptr<TemporaryDir> temp_dir_;
  arrow::internal::PlatformFilename debug_log_path_;
};

auto* azurite_env = ::testing::AddGlobalTestEnvironment(new AzuriteEnv);

AzuriteEnv* GetAzuriteEnv() {
  return ::arrow::internal::checked_cast<AzuriteEnv*>(azurite_env);
}

// Placeholder tests
// TODO: GH-18014 Remove once a proper test is added
TEST(AzureFileSystem, InitializeCredentials) {
  auto default_credential = std::make_shared<Azure::Identity::DefaultAzureCredential>();
  auto managed_identity_credential =
      std::make_shared<Azure::Identity::ManagedIdentityCredential>();
  auto service_principal_credential =
      std::make_shared<Azure::Identity::ClientSecretCredential>("tenant_id", "client_id",
                                                                "client_secret");
}

TEST(AzureFileSystem, OptionsCompare) {
  AzureOptions options;
  EXPECT_TRUE(options.Equals(options));
}

class AzureFileSystemTest : public ::testing::Test {
 public:
  std::shared_ptr<FileSystem> fs_;
  std::unique_ptr<Blobs::BlobServiceClient> blob_service_client_;
  std::unique_ptr<Files::DataLake::DataLakeServiceClient> datalake_service_client_;
  AzureOptions options_;
  std::mt19937_64 generator_;
  std::string container_name_;
  bool suite_skipped_ = false;

  AzureFileSystemTest() : generator_(std::random_device()()) {}

  virtual Result<AzureOptions> MakeOptions() = 0;

  void SetUp() override {
    auto options = MakeOptions();
    if (options.ok()) {
      options_ = *options;
    } else {
      suite_skipped_ = true;
      GTEST_SKIP() << options.status().message();
    }
    // Stop-gap solution before GH-39119 is fixed.
    container_name_ = "z" + RandomChars(31);
    blob_service_client_ = std::make_unique<Blobs::BlobServiceClient>(
        options_.account_blob_url, options_.storage_credentials_provider);
    datalake_service_client_ = std::make_unique<Files::DataLake::DataLakeServiceClient>(
        options_.account_dfs_url, options_.storage_credentials_provider);
    ASSERT_OK_AND_ASSIGN(fs_, AzureFileSystem::Make(options_));
    auto container_client = CreateContainer(container_name_);

    auto blob_client = container_client.GetBlockBlobClient(PreexistingObjectName());
    blob_client.UploadFrom(reinterpret_cast<const uint8_t*>(kLoremIpsum),
                           strlen(kLoremIpsum));
  }

  void TearDown() override {
    if (!suite_skipped_) {
      auto containers = blob_service_client_->ListBlobContainers();
      for (auto container : containers.BlobContainers) {
        auto container_client =
            blob_service_client_->GetBlobContainerClient(container.Name);
        container_client.DeleteIfExists();
      }
    }
  }

  Blobs::BlobContainerClient CreateContainer(const std::string& name) {
    auto container_client = blob_service_client_->GetBlobContainerClient(name);
    (void)container_client.CreateIfNotExists();
    return container_client;
  }

  Blobs::BlobClient CreateBlob(Blobs::BlobContainerClient& container_client,
                               const std::string& name, const std::string& data = "") {
    auto blob_client = container_client.GetBlockBlobClient(name);
    (void)blob_client.UploadFrom(reinterpret_cast<const uint8_t*>(data.data()),
                                 data.size());
    return blob_client;
  }

  std::string PreexistingContainerName() const { return container_name_; }

  std::string PreexistingContainerPath() const {
    return PreexistingContainerName() + '/';
  }

  static std::string PreexistingObjectName() { return "test-object-name"; }

  std::string PreexistingObjectPath() const {
    return PreexistingContainerPath() + PreexistingObjectName();
  }

  std::string NotFoundObjectPath() { return PreexistingContainerPath() + "not-found"; }

  std::string RandomLine(int lineno, std::size_t width) {
    auto line = std::to_string(lineno) + ":    ";
    line += RandomChars(width - line.size() - 1);
    line += '\n';
    return line;
  }

  std::size_t RandomIndex(std::size_t end) {
    return std::uniform_int_distribution<std::size_t>(0, end - 1)(generator_);
  }

  std::string RandomChars(std::size_t count) {
    auto const fillers = std::string("abcdefghijlkmnopqrstuvwxyz0123456789");
    std::uniform_int_distribution<std::size_t> d(0, fillers.size() - 1);
    std::string s;
    std::generate_n(std::back_inserter(s), count, [&] { return fillers[d(generator_)]; });
    return s;
  }

  std::string RandomContainerName() { return RandomChars(32); }

  std::string RandomDirectoryName() { return RandomChars(32); }

  void UploadLines(const std::vector<std::string>& lines, const char* path_to_file,
                   int total_size) {
    const auto path = PreexistingContainerPath() + path_to_file;
    ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(path, {}));
    const auto all_lines = std::accumulate(lines.begin(), lines.end(), std::string(""));
    ASSERT_OK(output->Write(all_lines));
    ASSERT_OK(output->Close());
  }

  void RunGetFileInfoObjectWithNestedStructureTest();
  void RunGetFileInfoObjectTest();

  struct HierarchicalPaths {
    std::string container;
    std::string directory;
    std::vector<std::string> sub_paths;
  };

  // Need to use "void" as the return type to use ASSERT_* in this method.
  void CreateHierarchicalData(HierarchicalPaths& paths) {
    const auto container_path = RandomContainerName();
    const auto directory_path =
        internal::ConcatAbstractPath(container_path, RandomDirectoryName());
    const auto sub_directory_path =
        internal::ConcatAbstractPath(directory_path, "new-sub");
    const auto sub_blob_path =
        internal::ConcatAbstractPath(sub_directory_path, "sub.txt");
    const auto top_blob_path = internal::ConcatAbstractPath(directory_path, "top.txt");
    ASSERT_OK(fs_->CreateDir(sub_directory_path, true));
    ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(sub_blob_path));
    ASSERT_OK(output->Write(std::string_view("sub")));
    ASSERT_OK(output->Close());
    ASSERT_OK_AND_ASSIGN(output, fs_->OpenOutputStream(top_blob_path));
    ASSERT_OK(output->Write(std::string_view("top")));
    ASSERT_OK(output->Close());

    AssertFileInfo(fs_.get(), container_path, FileType::Directory);
    AssertFileInfo(fs_.get(), directory_path, FileType::Directory);
    AssertFileInfo(fs_.get(), sub_directory_path, FileType::Directory);
    AssertFileInfo(fs_.get(), sub_blob_path, FileType::File);
    AssertFileInfo(fs_.get(), top_blob_path, FileType::File);

    paths.container = container_path;
    paths.directory = directory_path;
    paths.sub_paths = {
        sub_directory_path,
        sub_blob_path,
        top_blob_path,
    };
  }

  char const* kSubData = "sub data";
  char const* kSomeData = "some data";
  char const* kOtherData = "other data";

  void SetUpSmallFileSystemTree() {
    // Set up test containers
    CreateContainer("empty-container");
    auto container = CreateContainer("container");

    CreateBlob(container, "emptydir/");
    CreateBlob(container, "somedir/subdir/subfile", kSubData);
    CreateBlob(container, "somefile", kSomeData);
    // Add an explicit marker for a non-empty directory.
    CreateBlob(container, "otherdir/1/2/");
    // otherdir/{1/,2/,3/} are implicitly assumed to exist because of
    // the otherdir/1/2/3/otherfile blob.
    CreateBlob(container, "otherdir/1/2/3/otherfile", kOtherData);
  }

  void AssertInfoAllContainersRecursive(const std::vector<FileInfo>& infos) {
    ASSERT_EQ(infos.size(), 14);
    AssertFileInfo(infos[0], "container", FileType::Directory);
    AssertFileInfo(infos[1], "container/emptydir", FileType::Directory);
    AssertFileInfo(infos[2], "container/otherdir", FileType::Directory);
    AssertFileInfo(infos[3], "container/otherdir/1", FileType::Directory);
    AssertFileInfo(infos[4], "container/otherdir/1/2", FileType::Directory);
    AssertFileInfo(infos[5], "container/otherdir/1/2/3", FileType::Directory);
    AssertFileInfo(infos[6], "container/otherdir/1/2/3/otherfile", FileType::File,
                   strlen(kOtherData));
    AssertFileInfo(infos[7], "container/somedir", FileType::Directory);
    AssertFileInfo(infos[8], "container/somedir/subdir", FileType::Directory);
    AssertFileInfo(infos[9], "container/somedir/subdir/subfile", FileType::File,
                   strlen(kSubData));
    AssertFileInfo(infos[10], "container/somefile", FileType::File, strlen(kSomeData));
    AssertFileInfo(infos[11], "empty-container", FileType::Directory);
    AssertFileInfo(infos[12], PreexistingContainerName(), FileType::Directory);
    AssertFileInfo(infos[13], PreexistingObjectPath(), FileType::File);
  }
};

class AzuriteFileSystemTest : public AzureFileSystemTest {
  Result<AzureOptions> MakeOptions() override {
    EXPECT_THAT(GetAzuriteEnv(), NotNull());
    ARROW_EXPECT_OK(GetAzuriteEnv()->status());
    ARROW_ASSIGN_OR_RAISE(debug_log_start_, GetAzuriteEnv()->GetDebugLogSize());
    AzureOptions options;
    options.backend = AzureBackend::Azurite;
    ARROW_EXPECT_OK(options.ConfigureAccountKeyCredentials(
        GetAzuriteEnv()->account_name(), GetAzuriteEnv()->account_key()));
    return options;
  }

  void TearDown() override {
    AzureFileSystemTest::TearDown();
    if (HasFailure()) {
      // XXX: This may not include all logs in the target test because
      // Azurite doesn't flush debug logs immediately... You may want
      // to check the log manually...
      ARROW_IGNORE_EXPR(GetAzuriteEnv()->DumpDebugLog(debug_log_start_));
    }
  }

  int64_t debug_log_start_ = 0;
};

class AzureFlatNamespaceFileSystemTest : public AzureFileSystemTest {
  Result<AzureOptions> MakeOptions() override {
    AzureOptions options;
    const auto account_key = std::getenv("AZURE_FLAT_NAMESPACE_ACCOUNT_KEY");
    const auto account_name = std::getenv("AZURE_FLAT_NAMESPACE_ACCOUNT_NAME");
    if (account_key && account_name) {
      RETURN_NOT_OK(options.ConfigureAccountKeyCredentials(account_name, account_key));
      return options;
    }
    return Status::Cancelled(
        "Connection details not provided for a real flat namespace "
        "account.");
  }
};

// How to enable this test:
//
// You need an Azure account. You should be able to create a free
// account at https://azure.microsoft.com/en-gb/free/ . You should be
// able to create a storage account through the portal Web UI.
//
// See also the official document how to create a storage account:
// https://learn.microsoft.com/en-us/azure/storage/blobs/create-data-lake-storage-account
//
// A few suggestions on configuration:
//
// * Use Standard general-purpose v2 not premium
// * Use LRS redundancy
// * Obviously you need to enable hierarchical namespace.
// * Set the default access tier to hot
// * SFTP, NFS and file shares are not required.
class AzureHierarchicalNamespaceFileSystemTest : public AzureFileSystemTest {
  Result<AzureOptions> MakeOptions() override {
    AzureOptions options;
    const auto account_key = std::getenv("AZURE_HIERARCHICAL_NAMESPACE_ACCOUNT_KEY");
    const auto account_name = std::getenv("AZURE_HIERARCHICAL_NAMESPACE_ACCOUNT_NAME");
    if (account_key && account_name) {
      RETURN_NOT_OK(options.ConfigureAccountKeyCredentials(account_name, account_key));
      return options;
    }
    return Status::Cancelled(
        "Connection details not provided for a real hierarchical namespace "
        "account.");
  }
};

TEST_F(AzureFlatNamespaceFileSystemTest, DetectHierarchicalNamespace) {
  auto hierarchical_namespace = internal::HierarchicalNamespaceDetector();
  ASSERT_OK(hierarchical_namespace.Init(datalake_service_client_.get()));
  ASSERT_OK_AND_EQ(false, hierarchical_namespace.Enabled(PreexistingContainerName()));
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DetectHierarchicalNamespace) {
  auto hierarchical_namespace = internal::HierarchicalNamespaceDetector();
  ASSERT_OK(hierarchical_namespace.Init(datalake_service_client_.get()));
  ASSERT_OK_AND_EQ(true, hierarchical_namespace.Enabled(PreexistingContainerName()));
}

TEST_F(AzuriteFileSystemTest, DetectHierarchicalNamespace) {
  auto hierarchical_namespace = internal::HierarchicalNamespaceDetector();
  ASSERT_OK(hierarchical_namespace.Init(datalake_service_client_.get()));
  ASSERT_OK_AND_EQ(false, hierarchical_namespace.Enabled(PreexistingContainerName()));
}

TEST_F(AzuriteFileSystemTest, DetectHierarchicalNamespaceFailsWithMissingContainer) {
  auto hierarchical_namespace = internal::HierarchicalNamespaceDetector();
  ASSERT_OK(hierarchical_namespace.Init(datalake_service_client_.get()));
  ASSERT_NOT_OK(hierarchical_namespace.Enabled("nonexistent-container"));
}

TEST_F(AzuriteFileSystemTest, GetFileInfoAccount) {
  AssertFileInfo(fs_.get(), "", FileType::Directory);

  // URI
  ASSERT_RAISES(Invalid, fs_->GetFileInfo("abfs://"));
}

TEST_F(AzuriteFileSystemTest, GetFileInfoContainer) {
  AssertFileInfo(fs_.get(), PreexistingContainerName(), FileType::Directory);

  AssertFileInfo(fs_.get(), "nonexistent-container", FileType::NotFound);

  // URI
  ASSERT_RAISES(Invalid, fs_->GetFileInfo("abfs://" + PreexistingContainerName()));
}

void AzureFileSystemTest::RunGetFileInfoObjectWithNestedStructureTest() {
  // Adds detailed tests to handle cases of different edge cases
  // with directory naming conventions (e.g. with and without slashes).
  constexpr auto kObjectName = "test-object-dir/some_other_dir/another_dir/foo";
  ASSERT_OK_AND_ASSIGN(
      auto output,
      fs_->OpenOutputStream(PreexistingContainerPath() + kObjectName, /*metadata=*/{}));
  const std::string_view data(kLoremIpsum);
  ASSERT_OK(output->Write(data));
  ASSERT_OK(output->Close());

  // 0 is immediately after "/" lexicographically, ensure that this doesn't
  // cause unexpected issues.
  ASSERT_OK_AND_ASSIGN(output,
                       fs_->OpenOutputStream(
                           PreexistingContainerPath() + "test-object-dir/some_other_dir0",
                           /*metadata=*/{}));
  ASSERT_OK(output->Write(data));
  ASSERT_OK(output->Close());
  ASSERT_OK_AND_ASSIGN(
      output, fs_->OpenOutputStream(PreexistingContainerPath() + kObjectName + "0",
                                    /*metadata=*/{}));
  ASSERT_OK(output->Write(data));
  ASSERT_OK(output->Close());

  AssertFileInfo(fs_.get(), PreexistingContainerPath() + kObjectName, FileType::File);
  AssertFileInfo(fs_.get(), PreexistingContainerPath() + kObjectName + "/",
                 FileType::NotFound);
  AssertFileInfo(fs_.get(), PreexistingContainerPath() + "test-object-dir",
                 FileType::Directory);
  AssertFileInfo(fs_.get(), PreexistingContainerPath() + "test-object-dir/",
                 FileType::Directory);
  AssertFileInfo(fs_.get(), PreexistingContainerPath() + "test-object-dir/some_other_dir",
                 FileType::Directory);
  AssertFileInfo(fs_.get(),
                 PreexistingContainerPath() + "test-object-dir/some_other_dir/",
                 FileType::Directory);

  AssertFileInfo(fs_.get(), PreexistingContainerPath() + "test-object-di",
                 FileType::NotFound);
  AssertFileInfo(fs_.get(), PreexistingContainerPath() + "test-object-dir/some_other_di",
                 FileType::NotFound);
}

TEST_F(AzuriteFileSystemTest, GetFileInfoObjectWithNestedStructure) {
  RunGetFileInfoObjectWithNestedStructureTest();
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, GetFileInfoObjectWithNestedStructure) {
  RunGetFileInfoObjectWithNestedStructureTest();
  datalake_service_client_->GetFileSystemClient(PreexistingContainerName())
      .GetDirectoryClient("test-empty-object-dir")
      .Create();

  AssertFileInfo(fs_.get(), PreexistingContainerPath() + "test-empty-object-dir",
                 FileType::Directory);
}

void AzureFileSystemTest::RunGetFileInfoObjectTest() {
  auto object_properties =
      blob_service_client_->GetBlobContainerClient(PreexistingContainerName())
          .GetBlobClient(PreexistingObjectName())
          .GetProperties()
          .Value;

  AssertFileInfo(fs_.get(), PreexistingObjectPath(), FileType::File,
                 std::chrono::system_clock::time_point(object_properties.LastModified),
                 static_cast<int64_t>(object_properties.BlobSize));

  // URI
  ASSERT_RAISES(Invalid, fs_->GetFileInfo("abfs://" + PreexistingObjectName()));
}

TEST_F(AzuriteFileSystemTest, GetFileInfoObject) { RunGetFileInfoObjectTest(); }

TEST_F(AzureHierarchicalNamespaceFileSystemTest, GetFileInfoObject) {
  RunGetFileInfoObjectTest();
}

TEST_F(AzuriteFileSystemTest, GetFileInfoSelector) {
  SetUpSmallFileSystemTree();

  FileSelector select;
  std::vector<FileInfo> infos;

  // Root dir
  select.base_dir = "";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 3);
  ASSERT_EQ(infos, SortedInfos(infos));
  AssertFileInfo(infos[0], "container", FileType::Directory);
  AssertFileInfo(infos[1], "empty-container", FileType::Directory);
  AssertFileInfo(infos[2], container_name_, FileType::Directory);

  // Empty container
  select.base_dir = "empty-container";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);
  // Nonexistent container
  select.base_dir = "nonexistent-container";
  ASSERT_RAISES(IOError, fs_->GetFileInfo(select));
  select.allow_not_found = true;
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);
  select.allow_not_found = false;
  // Non-empty container
  select.base_dir = "container";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos, SortedInfos(infos));
  ASSERT_EQ(infos.size(), 4);
  AssertFileInfo(infos[0], "container/emptydir", FileType::Directory);
  AssertFileInfo(infos[1], "container/otherdir", FileType::Directory);
  AssertFileInfo(infos[2], "container/somedir", FileType::Directory);
  AssertFileInfo(infos[3], "container/somefile", FileType::File, 9);

  // Empty "directory"
  select.base_dir = "container/emptydir";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);
  // Non-empty "directories"
  select.base_dir = "container/somedir";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 1);
  AssertFileInfo(infos[0], "container/somedir/subdir", FileType::Directory);
  select.base_dir = "container/somedir/subdir";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 1);
  AssertFileInfo(infos[0], "container/somedir/subdir/subfile", FileType::File, 8);
  // Nonexistent
  select.base_dir = "container/nonexistent";
  ASSERT_RAISES(IOError, fs_->GetFileInfo(select));
  select.allow_not_found = true;
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);
  select.allow_not_found = false;

  // Trailing slashes
  select.base_dir = "empty-container/";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);
  select.base_dir = "nonexistent-container/";
  ASSERT_RAISES(IOError, fs_->GetFileInfo(select));
  select.base_dir = "container/";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos, SortedInfos(infos));
  ASSERT_EQ(infos.size(), 4);
}

TEST_F(AzuriteFileSystemTest, GetFileInfoSelectorRecursive) {
  SetUpSmallFileSystemTree();

  FileSelector select;
  select.recursive = true;

  std::vector<FileInfo> infos;
  // Root dir
  select.base_dir = "";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 14);
  ASSERT_EQ(infos, SortedInfos(infos));
  AssertInfoAllContainersRecursive(infos);

  // Empty container
  select.base_dir = "empty-container";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);

  // Non-empty container
  select.base_dir = "container";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos, SortedInfos(infos));
  ASSERT_EQ(infos.size(), 10);
  AssertFileInfo(infos[0], "container/emptydir", FileType::Directory);
  AssertFileInfo(infos[1], "container/otherdir", FileType::Directory);
  AssertFileInfo(infos[2], "container/otherdir/1", FileType::Directory);
  AssertFileInfo(infos[3], "container/otherdir/1/2", FileType::Directory);
  AssertFileInfo(infos[4], "container/otherdir/1/2/3", FileType::Directory);
  AssertFileInfo(infos[5], "container/otherdir/1/2/3/otherfile", FileType::File, 10);
  AssertFileInfo(infos[6], "container/somedir", FileType::Directory);
  AssertFileInfo(infos[7], "container/somedir/subdir", FileType::Directory);
  AssertFileInfo(infos[8], "container/somedir/subdir/subfile", FileType::File, 8);
  AssertFileInfo(infos[9], "container/somefile", FileType::File, 9);

  // Empty "directory"
  select.base_dir = "container/emptydir";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);

  // Non-empty "directories"
  select.base_dir = "container/somedir";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos, SortedInfos(infos));
  ASSERT_EQ(infos.size(), 2);
  AssertFileInfo(infos[0], "container/somedir/subdir", FileType::Directory);
  AssertFileInfo(infos[1], "container/somedir/subdir/subfile", FileType::File, 8);

  select.base_dir = "container/otherdir";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos, SortedInfos(infos));
  ASSERT_EQ(infos.size(), 4);
  AssertFileInfo(infos[0], "container/otherdir/1", FileType::Directory);
  AssertFileInfo(infos[1], "container/otherdir/1/2", FileType::Directory);
  AssertFileInfo(infos[2], "container/otherdir/1/2/3", FileType::Directory);
  AssertFileInfo(infos[3], "container/otherdir/1/2/3/otherfile", FileType::File, 10);
}

TEST_F(AzuriteFileSystemTest, GetFileInfoSelectorExplicitImplicitDirDedup) {
  {
    auto container = CreateContainer("container");
    CreateBlob(container, "mydir/emptydir1/");
    CreateBlob(container, "mydir/emptydir2/");
    CreateBlob(container, "mydir/nonemptydir1/");  // explicit dir marker
    CreateBlob(container, "mydir/nonemptydir1/somefile", kSomeData);
    CreateBlob(container, "mydir/nonemptydir2/somefile", kSomeData);
  }
  std::vector<FileInfo> infos;

  FileSelector select;  // non-recursive
  select.base_dir = "container";

  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 1);
  ASSERT_EQ(infos, SortedInfos(infos));
  AssertFileInfo(infos[0], "container/mydir", FileType::Directory);

  select.base_dir = "container/mydir";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 4);
  ASSERT_EQ(infos, SortedInfos(infos));
  AssertFileInfo(infos[0], "container/mydir/emptydir1", FileType::Directory);
  AssertFileInfo(infos[1], "container/mydir/emptydir2", FileType::Directory);
  AssertFileInfo(infos[2], "container/mydir/nonemptydir1", FileType::Directory);
  AssertFileInfo(infos[3], "container/mydir/nonemptydir2", FileType::Directory);

  select.base_dir = "container/mydir/emptydir1";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);

  select.base_dir = "container/mydir/emptydir2";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 0);

  select.base_dir = "container/mydir/nonemptydir1";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 1);
  AssertFileInfo(infos[0], "container/mydir/nonemptydir1/somefile", FileType::File);

  select.base_dir = "container/mydir/nonemptydir2";
  ASSERT_OK_AND_ASSIGN(infos, fs_->GetFileInfo(select));
  ASSERT_EQ(infos.size(), 1);
  AssertFileInfo(infos[0], "container/mydir/nonemptydir2/somefile", FileType::File);
}

TEST_F(AzuriteFileSystemTest, CreateDirFailureNoContainer) {
  ASSERT_RAISES(Invalid, fs_->CreateDir("", false));
}

TEST_F(AzuriteFileSystemTest, CreateDirSuccessContainerOnly) {
  auto container_name = RandomContainerName();
  ASSERT_OK(fs_->CreateDir(container_name, false));
  arrow::fs::AssertFileInfo(fs_.get(), container_name, FileType::Directory);
}

TEST_F(AzuriteFileSystemTest, CreateDirSuccessContainerAndDirectory) {
  const auto path = PreexistingContainerPath() + RandomDirectoryName();
  ASSERT_OK(fs_->CreateDir(path, false));
  // There is only virtual directory without hierarchical namespace
  // support. So the CreateDir() does nothing.
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::NotFound);
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, CreateDirSuccessContainerAndDirectory) {
  const auto path = PreexistingContainerPath() + RandomDirectoryName();
  ASSERT_OK(fs_->CreateDir(path, false));
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::Directory);
}

TEST_F(AzuriteFileSystemTest, CreateDirFailureDirectoryWithMissingContainer) {
  const auto path = std::string("not-a-container/new-directory");
  ASSERT_RAISES(IOError, fs_->CreateDir(path, false));
}

TEST_F(AzuriteFileSystemTest, CreateDirRecursiveFailureNoContainer) {
  ASSERT_RAISES(Invalid, fs_->CreateDir("", true));
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, CreateDirRecursiveSuccessContainerOnly) {
  auto container_name = RandomContainerName();
  ASSERT_OK(fs_->CreateDir(container_name, true));
  arrow::fs::AssertFileInfo(fs_.get(), container_name, FileType::Directory);
}

TEST_F(AzuriteFileSystemTest, CreateDirRecursiveSuccessContainerOnly) {
  auto container_name = RandomContainerName();
  ASSERT_OK(fs_->CreateDir(container_name, true));
  arrow::fs::AssertFileInfo(fs_.get(), container_name, FileType::Directory);
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, CreateDirRecursiveSuccessDirectoryOnly) {
  const auto parent = PreexistingContainerPath() + RandomDirectoryName();
  const auto path = internal::ConcatAbstractPath(parent, "new-sub");
  ASSERT_OK(fs_->CreateDir(path, true));
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::Directory);
  arrow::fs::AssertFileInfo(fs_.get(), parent, FileType::Directory);
}

TEST_F(AzuriteFileSystemTest, CreateDirRecursiveSuccessDirectoryOnly) {
  const auto parent = PreexistingContainerPath() + RandomDirectoryName();
  const auto path = internal::ConcatAbstractPath(parent, "new-sub");
  ASSERT_OK(fs_->CreateDir(path, true));
  // There is only virtual directory without hierarchical namespace
  // support. So the CreateDir() does nothing.
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::NotFound);
  arrow::fs::AssertFileInfo(fs_.get(), parent, FileType::NotFound);
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest,
       CreateDirRecursiveSuccessContainerAndDirectory) {
  auto container_name = RandomContainerName();
  const auto parent = internal::ConcatAbstractPath(container_name, RandomDirectoryName());
  const auto path = internal::ConcatAbstractPath(parent, "new-sub");
  ASSERT_OK(fs_->CreateDir(path, true));
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::Directory);
  arrow::fs::AssertFileInfo(fs_.get(), parent, FileType::Directory);
  arrow::fs::AssertFileInfo(fs_.get(), container_name, FileType::Directory);
}

TEST_F(AzuriteFileSystemTest, CreateDirRecursiveSuccessContainerAndDirectory) {
  auto container_name = RandomContainerName();
  const auto parent = internal::ConcatAbstractPath(container_name, RandomDirectoryName());
  const auto path = internal::ConcatAbstractPath(parent, "new-sub");
  ASSERT_OK(fs_->CreateDir(path, true));
  // There is only virtual directory without hierarchical namespace
  // support. So the CreateDir() does nothing.
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::NotFound);
  arrow::fs::AssertFileInfo(fs_.get(), parent, FileType::NotFound);
  arrow::fs::AssertFileInfo(fs_.get(), container_name, FileType::Directory);
}

TEST_F(AzuriteFileSystemTest, CreateDirUri) {
  ASSERT_RAISES(Invalid, fs_->CreateDir("abfs://" + RandomContainerName(), true));
}

TEST_F(AzuriteFileSystemTest, DeleteDirSuccessContainer) {
  const auto container_name = RandomContainerName();
  ASSERT_OK(fs_->CreateDir(container_name));
  arrow::fs::AssertFileInfo(fs_.get(), container_name, FileType::Directory);
  ASSERT_OK(fs_->DeleteDir(container_name));
  arrow::fs::AssertFileInfo(fs_.get(), container_name, FileType::NotFound);
}

TEST_F(AzuriteFileSystemTest, DeleteDirSuccessEmpty) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  // There is only virtual directory without hierarchical namespace
  // support. So the CreateDir() and DeleteDir() do nothing.
  ASSERT_OK(fs_->CreateDir(directory_path));
  arrow::fs::AssertFileInfo(fs_.get(), directory_path, FileType::NotFound);
  ASSERT_OK(fs_->DeleteDir(directory_path));
  arrow::fs::AssertFileInfo(fs_.get(), directory_path, FileType::NotFound);
}

TEST_F(AzuriteFileSystemTest, DeleteDirSuccessNonexistent) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  // There is only virtual directory without hierarchical namespace
  // support. So the DeleteDir() for nonexistent directory does nothing.
  ASSERT_OK(fs_->DeleteDir(directory_path));
  arrow::fs::AssertFileInfo(fs_.get(), directory_path, FileType::NotFound);
}

TEST_F(AzuriteFileSystemTest, DeleteDirSuccessHaveBlobs) {
#ifdef __APPLE__
  GTEST_SKIP() << "This test fails by an Azurite problem: "
                  "https://github.com/Azure/Azurite/pull/2302";
#endif
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  // We must use 257 or more blobs here to test pagination of ListBlobs().
  // Because we can't add 257 or more delete blob requests to one SubmitBatch().
  int64_t n_blobs = 257;
  for (int64_t i = 0; i < n_blobs; ++i) {
    const auto blob_path =
        internal::ConcatAbstractPath(directory_path, std::to_string(i) + ".txt");
    ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(blob_path));
    ASSERT_OK(output->Write(std::string_view(std::to_string(i))));
    ASSERT_OK(output->Close());
    arrow::fs::AssertFileInfo(fs_.get(), blob_path, FileType::File);
  }
  ASSERT_OK(fs_->DeleteDir(directory_path));
  for (int64_t i = 0; i < n_blobs; ++i) {
    const auto blob_path =
        internal::ConcatAbstractPath(directory_path, std::to_string(i) + ".txt");
    arrow::fs::AssertFileInfo(fs_.get(), blob_path, FileType::NotFound);
  }
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DeleteDirSuccessEmpty) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  ASSERT_OK(fs_->CreateDir(directory_path, true));
  arrow::fs::AssertFileInfo(fs_.get(), directory_path, FileType::Directory);
  ASSERT_OK(fs_->DeleteDir(directory_path));
  arrow::fs::AssertFileInfo(fs_.get(), directory_path, FileType::NotFound);
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DeleteDirFailureNonexistent) {
  const auto path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  ASSERT_RAISES(IOError, fs_->DeleteDir(path));
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DeleteDirSuccessHaveBlob) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  const auto blob_path = internal::ConcatAbstractPath(directory_path, "hello.txt");
  ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(blob_path));
  ASSERT_OK(output->Write(std::string_view("hello")));
  ASSERT_OK(output->Close());
  arrow::fs::AssertFileInfo(fs_.get(), blob_path, FileType::File);
  ASSERT_OK(fs_->DeleteDir(directory_path));
  arrow::fs::AssertFileInfo(fs_.get(), blob_path, FileType::NotFound);
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DeleteDirSuccessHaveDirectory) {
  const auto parent =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  const auto path = internal::ConcatAbstractPath(parent, "new-sub");
  ASSERT_OK(fs_->CreateDir(path, true));
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::Directory);
  arrow::fs::AssertFileInfo(fs_.get(), parent, FileType::Directory);
  ASSERT_OK(fs_->DeleteDir(parent));
  arrow::fs::AssertFileInfo(fs_.get(), path, FileType::NotFound);
  arrow::fs::AssertFileInfo(fs_.get(), parent, FileType::NotFound);
}

TEST_F(AzuriteFileSystemTest, DeleteDirUri) {
  ASSERT_RAISES(Invalid, fs_->DeleteDir("abfs://" + PreexistingContainerPath()));
}

TEST_F(AzuriteFileSystemTest, DeleteDirContentsSuccessContainer) {
#ifdef __APPLE__
  GTEST_SKIP() << "This test fails by an Azurite problem: "
                  "https://github.com/Azure/Azurite/pull/2302";
#endif
  HierarchicalPaths paths;
  CreateHierarchicalData(paths);
  ASSERT_OK(fs_->DeleteDirContents(paths.container));
  arrow::fs::AssertFileInfo(fs_.get(), paths.container, FileType::Directory);
  arrow::fs::AssertFileInfo(fs_.get(), paths.directory, FileType::NotFound);
  for (const auto& sub_path : paths.sub_paths) {
    arrow::fs::AssertFileInfo(fs_.get(), sub_path, FileType::NotFound);
  }
}

TEST_F(AzuriteFileSystemTest, DeleteDirContentsSuccessDirectory) {
#ifdef __APPLE__
  GTEST_SKIP() << "This test fails by an Azurite problem: "
                  "https://github.com/Azure/Azurite/pull/2302";
#endif
  HierarchicalPaths paths;
  CreateHierarchicalData(paths);
  ASSERT_OK(fs_->DeleteDirContents(paths.directory));
  // GH-38772: We may change this to FileType::Directory.
  arrow::fs::AssertFileInfo(fs_.get(), paths.directory, FileType::NotFound);
  for (const auto& sub_path : paths.sub_paths) {
    arrow::fs::AssertFileInfo(fs_.get(), sub_path, FileType::NotFound);
  }
}

TEST_F(AzuriteFileSystemTest, DeleteDirContentsSuccessNonexistent) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  ASSERT_OK(fs_->DeleteDirContents(directory_path, true));
  arrow::fs::AssertFileInfo(fs_.get(), directory_path, FileType::NotFound);
}

TEST_F(AzuriteFileSystemTest, DeleteDirContentsFailureNonexistent) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  ASSERT_RAISES(IOError, fs_->DeleteDirContents(directory_path, false));
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DeleteDirContentsSuccessExist) {
  HierarchicalPaths paths;
  CreateHierarchicalData(paths);
  ASSERT_OK(fs_->DeleteDirContents(paths.directory));
  arrow::fs::AssertFileInfo(fs_.get(), paths.directory, FileType::Directory);
  for (const auto& sub_path : paths.sub_paths) {
    arrow::fs::AssertFileInfo(fs_.get(), sub_path, FileType::NotFound);
  }
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DeleteDirContentsSuccessNonexistent) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  ASSERT_OK(fs_->DeleteDirContents(directory_path, true));
  arrow::fs::AssertFileInfo(fs_.get(), directory_path, FileType::NotFound);
}

TEST_F(AzureHierarchicalNamespaceFileSystemTest, DeleteDirContentsFailureNonexistent) {
  const auto directory_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), RandomDirectoryName());
  ASSERT_RAISES(IOError, fs_->DeleteDirContents(directory_path, false));
}

TEST_F(AzuriteFileSystemTest, CopyFileSuccessDestinationNonexistent) {
  const auto destination_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), "copy-destionation");
  ASSERT_OK(fs_->CopyFile(PreexistingObjectPath(), destination_path));
  ASSERT_OK_AND_ASSIGN(auto info, fs_->GetFileInfo(destination_path));
  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputStream(info));
  ASSERT_OK_AND_ASSIGN(auto buffer, stream->Read(1024));
  EXPECT_EQ(kLoremIpsum, buffer->ToString());
}

TEST_F(AzuriteFileSystemTest, CopyFileSuccessDestinationSame) {
  ASSERT_OK(fs_->CopyFile(PreexistingObjectPath(), PreexistingObjectPath()));
  ASSERT_OK_AND_ASSIGN(auto info, fs_->GetFileInfo(PreexistingObjectPath()));
  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputStream(info));
  ASSERT_OK_AND_ASSIGN(auto buffer, stream->Read(1024));
  EXPECT_EQ(kLoremIpsum, buffer->ToString());
}

TEST_F(AzuriteFileSystemTest, CopyFileFailureDestinationTrailingSlash) {
  ASSERT_RAISES(IOError,
                fs_->CopyFile(PreexistingObjectPath(),
                              internal::EnsureTrailingSlash(PreexistingObjectPath())));
}

TEST_F(AzuriteFileSystemTest, CopyFileFailureSourceNonexistent) {
  const auto destination_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), "copy-destionation");
  ASSERT_RAISES(IOError, fs_->CopyFile(NotFoundObjectPath(), destination_path));
}

TEST_F(AzuriteFileSystemTest, CopyFileFailureDestinationParentNonexistent) {
  const auto destination_path =
      internal::ConcatAbstractPath(RandomContainerName(), "copy-destionation");
  ASSERT_RAISES(IOError, fs_->CopyFile(PreexistingObjectPath(), destination_path));
}

TEST_F(AzuriteFileSystemTest, CopyFileUri) {
  const auto destination_path =
      internal::ConcatAbstractPath(PreexistingContainerName(), "copy-destionation");
  ASSERT_RAISES(Invalid,
                fs_->CopyFile("abfs://" + PreexistingObjectPath(), destination_path));
  ASSERT_RAISES(Invalid,
                fs_->CopyFile(PreexistingObjectPath(), "abfs://" + destination_path));
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamString) {
  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(PreexistingObjectPath()));

  ASSERT_OK_AND_ASSIGN(auto buffer, stream->Read(1024));
  EXPECT_EQ(buffer->ToString(), kLoremIpsum);
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamStringBuffers) {
  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(PreexistingObjectPath()));

  std::string contents;
  std::shared_ptr<Buffer> buffer;
  do {
    ASSERT_OK_AND_ASSIGN(buffer, stream->Read(16));
    contents.append(buffer->ToString());
  } while (buffer && buffer->size() != 0);

  EXPECT_EQ(contents, kLoremIpsum);
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamInfo) {
  ASSERT_OK_AND_ASSIGN(auto info, fs_->GetFileInfo(PreexistingObjectPath()));

  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(info));

  ASSERT_OK_AND_ASSIGN(auto buffer, stream->Read(1024));
  EXPECT_EQ(buffer->ToString(), kLoremIpsum);
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamEmpty) {
  const auto path_to_file = "empty-object.txt";
  const auto path = PreexistingContainerPath() + path_to_file;
  blob_service_client_->GetBlobContainerClient(PreexistingContainerName())
      .GetBlockBlobClient(path_to_file)
      .UploadFrom(nullptr, 0);

  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputStream(path));
  std::array<char, 1024> buffer{};
  std::int64_t size;
  ASSERT_OK_AND_ASSIGN(size, stream->Read(buffer.size(), buffer.data()));
  EXPECT_EQ(size, 0);
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamNotFound) {
  ASSERT_RAISES(IOError, fs_->OpenInputStream(NotFoundObjectPath()));
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamInfoInvalid) {
  ASSERT_OK_AND_ASSIGN(auto info, fs_->GetFileInfo(PreexistingContainerPath()));
  ASSERT_RAISES(IOError, fs_->OpenInputStream(info));

  ASSERT_OK_AND_ASSIGN(auto info2, fs_->GetFileInfo(NotFoundObjectPath()));
  ASSERT_RAISES(IOError, fs_->OpenInputStream(info2));
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamUri) {
  ASSERT_RAISES(Invalid, fs_->OpenInputStream("abfs://" + PreexistingObjectPath()));
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamTrailingSlash) {
  ASSERT_RAISES(IOError, fs_->OpenInputStream(PreexistingObjectPath() + '/'));
}

namespace {
std::shared_ptr<const KeyValueMetadata> NormalizerKeyValueMetadata(
    std::shared_ptr<const KeyValueMetadata> metadata) {
  auto normalized = std::make_shared<KeyValueMetadata>();
  for (int64_t i = 0; i < metadata->size(); ++i) {
    auto key = metadata->key(i);
    auto value = metadata->value(i);
    if (key == "Content-Hash") {
      std::vector<uint8_t> output;
      output.reserve(value.size() / 2);
      if (ParseHexValues(value, output.data()).ok()) {
        // Valid value
        value = std::string(value.size(), 'F');
      }
    } else if (key == "Last-Modified" || key == "Created-On" ||
               key == "Access-Tier-Changed-On") {
      auto parser = TimestampParser::MakeISO8601();
      int64_t output;
      if ((*parser)(value.data(), value.size(), TimeUnit::NANO, &output)) {
        // Valid value
        value = "2023-10-31T08:15:20Z";
      }
    } else if (key == "ETag") {
      if (arrow::internal::StartsWith(value, "\"") &&
          arrow::internal::EndsWith(value, "\"")) {
        // Valid value
        value = "\"ETagValue\"";
      }
    }
    normalized->Append(key, value);
  }
  return normalized;
}
};  // namespace

TEST_F(AzuriteFileSystemTest, OpenInputStreamReadMetadata) {
  std::shared_ptr<io::InputStream> stream;
  ASSERT_OK_AND_ASSIGN(stream, fs_->OpenInputStream(PreexistingObjectPath()));

  std::shared_ptr<const KeyValueMetadata> actual;
  ASSERT_OK_AND_ASSIGN(actual, stream->ReadMetadata());
  ASSERT_EQ(
      "\n"
      "-- metadata --\n"
      "Content-Type: application/octet-stream\n"
      "Content-Encoding: \n"
      "Content-Language: \n"
      "Content-Hash: FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF\n"
      "Content-Disposition: \n"
      "Cache-Control: \n"
      "Last-Modified: 2023-10-31T08:15:20Z\n"
      "Created-On: 2023-10-31T08:15:20Z\n"
      "Blob-Type: BlockBlob\n"
      "Lease-State: available\n"
      "Lease-Status: unlocked\n"
      "Content-Length: 447\n"
      "ETag: \"ETagValue\"\n"
      "IsServerEncrypted: true\n"
      "Access-Tier: Hot\n"
      "Is-Access-Tier-Inferred: true\n"
      "Access-Tier-Changed-On: 2023-10-31T08:15:20Z\n"
      "Has-Legal-Hold: false",
      NormalizerKeyValueMetadata(actual)->ToString());
}

TEST_F(AzuriteFileSystemTest, OpenInputStreamClosed) {
  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputStream(PreexistingObjectPath()));
  ASSERT_OK(stream->Close());
  std::array<char, 16> buffer{};
  ASSERT_RAISES(Invalid, stream->Read(buffer.size(), buffer.data()));
  ASSERT_RAISES(Invalid, stream->Read(buffer.size()));
  ASSERT_RAISES(Invalid, stream->Tell());
}

TEST_F(AzuriteFileSystemTest, TestWriteMetadata) {
  options_.default_metadata = arrow::key_value_metadata({{"foo", "bar"}});

  ASSERT_OK_AND_ASSIGN(auto fs_with_defaults, AzureFileSystem::Make(options_));
  std::string path = "object_with_defaults";
  auto location = PreexistingContainerPath() + path;
  ASSERT_OK_AND_ASSIGN(auto output,
                       fs_with_defaults->OpenOutputStream(location, /*metadata=*/{}));
  const std::string_view expected(kLoremIpsum);
  ASSERT_OK(output->Write(expected));
  ASSERT_OK(output->Close());

  // Verify the metadata has been set.
  auto blob_metadata =
      blob_service_client_->GetBlobContainerClient(PreexistingContainerName())
          .GetBlockBlobClient(path)
          .GetProperties()
          .Value.Metadata;
  EXPECT_EQ(Azure::Core::CaseInsensitiveMap{std::make_pair("foo", "bar")}, blob_metadata);

  // Check that explicit metadata overrides the defaults.
  ASSERT_OK_AND_ASSIGN(
      output, fs_with_defaults->OpenOutputStream(
                  location, /*metadata=*/arrow::key_value_metadata({{"bar", "foo"}})));
  ASSERT_OK(output->Write(expected));
  ASSERT_OK(output->Close());
  blob_metadata = blob_service_client_->GetBlobContainerClient(PreexistingContainerName())
                      .GetBlockBlobClient(path)
                      .GetProperties()
                      .Value.Metadata;
  // Defaults are overwritten and not merged.
  EXPECT_EQ(Azure::Core::CaseInsensitiveMap{std::make_pair("bar", "foo")}, blob_metadata);
}

TEST_F(AzuriteFileSystemTest, OpenOutputStreamSmall) {
  const auto path = PreexistingContainerPath() + "test-write-object";
  ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(path, {}));
  const std::string_view expected(kLoremIpsum);
  ASSERT_OK(output->Write(expected));
  ASSERT_OK(output->Close());

  // Verify we can read the object back.
  ASSERT_OK_AND_ASSIGN(auto input, fs_->OpenInputStream(path));

  std::array<char, 1024> inbuf{};
  ASSERT_OK_AND_ASSIGN(auto size, input->Read(inbuf.size(), inbuf.data()));

  EXPECT_EQ(expected, std::string_view(inbuf.data(), size));
}

TEST_F(AzuriteFileSystemTest, OpenOutputStreamLarge) {
  const auto path = PreexistingContainerPath() + "test-write-object";
  ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(path, {}));
  std::array<std::int64_t, 3> sizes{257 * 1024, 258 * 1024, 259 * 1024};
  std::array<std::string, 3> buffers{
      std::string(sizes[0], 'A'),
      std::string(sizes[1], 'B'),
      std::string(sizes[2], 'C'),
  };
  auto expected = std::int64_t{0};
  for (auto i = 0; i != 3; ++i) {
    ASSERT_OK(output->Write(buffers[i]));
    expected += sizes[i];
    ASSERT_EQ(expected, output->Tell());
  }
  ASSERT_OK(output->Close());

  // Verify we can read the object back.
  ASSERT_OK_AND_ASSIGN(auto input, fs_->OpenInputStream(path));

  std::string contents;
  std::shared_ptr<Buffer> buffer;
  do {
    ASSERT_OK_AND_ASSIGN(buffer, input->Read(128 * 1024));
    ASSERT_TRUE(buffer);
    contents.append(buffer->ToString());
  } while (buffer->size() != 0);

  EXPECT_EQ(contents, buffers[0] + buffers[1] + buffers[2]);
}

TEST_F(AzuriteFileSystemTest, OpenOutputStreamTruncatesExistingFile) {
  const auto path = PreexistingContainerPath() + "test-write-object";
  ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(path, {}));
  const std::string_view expected0("Existing blob content");
  ASSERT_OK(output->Write(expected0));
  ASSERT_OK(output->Close());

  // Check that the initial content has been written - if not this test is not achieving
  // what it's meant to.
  ASSERT_OK_AND_ASSIGN(auto input, fs_->OpenInputStream(path));

  std::array<char, 1024> inbuf{};
  ASSERT_OK_AND_ASSIGN(auto size, input->Read(inbuf.size(), inbuf.data()));
  EXPECT_EQ(expected0, std::string_view(inbuf.data(), size));

  ASSERT_OK_AND_ASSIGN(output, fs_->OpenOutputStream(path, {}));
  const std::string_view expected1(kLoremIpsum);
  ASSERT_OK(output->Write(expected1));
  ASSERT_OK(output->Close());

  // Verify that the initial content has been overwritten.
  ASSERT_OK_AND_ASSIGN(input, fs_->OpenInputStream(path));
  ASSERT_OK_AND_ASSIGN(size, input->Read(inbuf.size(), inbuf.data()));
  EXPECT_EQ(expected1, std::string_view(inbuf.data(), size));
}

TEST_F(AzuriteFileSystemTest, OpenAppendStreamDoesNotTruncateExistingFile) {
  const auto path = PreexistingContainerPath() + "test-write-object";
  ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(path, {}));
  const std::string_view expected0("Existing blob content");
  ASSERT_OK(output->Write(expected0));
  ASSERT_OK(output->Close());

  // Check that the initial content has been written - if not this test is not achieving
  // what it's meant to.
  ASSERT_OK_AND_ASSIGN(auto input, fs_->OpenInputStream(path));

  std::array<char, 1024> inbuf{};
  ASSERT_OK_AND_ASSIGN(auto size, input->Read(inbuf.size(), inbuf.data()));
  EXPECT_EQ(expected0, std::string_view(inbuf.data()));

  ASSERT_OK_AND_ASSIGN(output, fs_->OpenAppendStream(path, {}));
  const std::string_view expected1(kLoremIpsum);
  ASSERT_OK(output->Write(expected1));
  ASSERT_OK(output->Close());

  // Verify that the initial content has not been overwritten and that the block from
  // the other client was not committed.
  ASSERT_OK_AND_ASSIGN(input, fs_->OpenInputStream(path));
  ASSERT_OK_AND_ASSIGN(size, input->Read(inbuf.size(), inbuf.data()));
  EXPECT_EQ(std::string(inbuf.data(), size),
            std::string(expected0) + std::string(expected1));
}

TEST_F(AzuriteFileSystemTest, OpenOutputStreamClosed) {
  const auto path = internal::ConcatAbstractPath(PreexistingContainerName(),
                                                 "open-output-stream-closed.txt");
  ASSERT_OK_AND_ASSIGN(auto output, fs_->OpenOutputStream(path, {}));
  ASSERT_OK(output->Close());
  ASSERT_RAISES(Invalid, output->Write(kLoremIpsum, std::strlen(kLoremIpsum)));
  ASSERT_RAISES(Invalid, output->Flush());
  ASSERT_RAISES(Invalid, output->Tell());
}

TEST_F(AzuriteFileSystemTest, OpenOutputStreamUri) {
  const auto path = internal::ConcatAbstractPath(PreexistingContainerName(),
                                                 "open-output-stream-uri.txt");
  ASSERT_RAISES(Invalid, fs_->OpenInputStream("abfs://" + path));
}

TEST_F(AzuriteFileSystemTest, OpenInputFileMixedReadVsReadAt) {
  // Create a file large enough to make the random access tests non-trivial.
  auto constexpr kLineWidth = 100;
  auto constexpr kLineCount = 4096;
  std::vector<std::string> lines(kLineCount);
  int lineno = 0;
  std::generate_n(lines.begin(), lines.size(),
                  [&] { return RandomLine(++lineno, kLineWidth); });

  const auto path_to_file = "OpenInputFileMixedReadVsReadAt/object-name";
  const auto path = PreexistingContainerPath() + path_to_file;

  UploadLines(lines, path_to_file, kLineCount * kLineWidth);

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(path));
  for (int i = 0; i != 32; ++i) {
    SCOPED_TRACE("Iteration " + std::to_string(i));
    // Verify sequential reads work as expected.
    std::array<char, kLineWidth> buffer{};
    std::int64_t size;
    {
      ASSERT_OK_AND_ASSIGN(auto actual, file->Read(kLineWidth));
      EXPECT_EQ(lines[2 * i], actual->ToString());
    }
    {
      ASSERT_OK_AND_ASSIGN(size, file->Read(buffer.size(), buffer.data()));
      EXPECT_EQ(size, kLineWidth);
      auto actual = std::string{buffer.begin(), buffer.end()};
      EXPECT_EQ(lines[2 * i + 1], actual);
    }

    // Verify random reads interleave too.
    auto const index = RandomIndex(kLineCount);
    auto const position = index * kLineWidth;
    ASSERT_OK_AND_ASSIGN(size, file->ReadAt(position, buffer.size(), buffer.data()));
    EXPECT_EQ(size, kLineWidth);
    auto actual = std::string{buffer.begin(), buffer.end()};
    EXPECT_EQ(lines[index], actual);

    // Verify random reads using buffers work.
    ASSERT_OK_AND_ASSIGN(auto b, file->ReadAt(position, kLineWidth));
    EXPECT_EQ(lines[index], b->ToString());
  }
}

TEST_F(AzuriteFileSystemTest, OpenInputFileRandomSeek) {
  // Create a file large enough to make the random access tests non-trivial.
  auto constexpr kLineWidth = 100;
  auto constexpr kLineCount = 4096;
  std::vector<std::string> lines(kLineCount);
  int lineno = 0;
  std::generate_n(lines.begin(), lines.size(),
                  [&] { return RandomLine(++lineno, kLineWidth); });

  const auto path_to_file = "OpenInputFileRandomSeek/object-name";
  const auto path = PreexistingContainerPath() + path_to_file;
  std::shared_ptr<io::OutputStream> output;

  UploadLines(lines, path_to_file, kLineCount * kLineWidth);

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(path));
  for (int i = 0; i != 32; ++i) {
    SCOPED_TRACE("Iteration " + std::to_string(i));
    // Verify sequential reads work as expected.
    auto const index = RandomIndex(kLineCount);
    auto const position = index * kLineWidth;
    ASSERT_OK(file->Seek(position));
    ASSERT_OK_AND_ASSIGN(auto actual, file->Read(kLineWidth));
    EXPECT_EQ(lines[index], actual->ToString());
  }
}

TEST_F(AzuriteFileSystemTest, OpenInputFileIoContext) {
  // Create a test file.
  const auto path_to_file = "OpenInputFileIoContext/object-name";
  const auto path = PreexistingContainerPath() + path_to_file;
  const std::string contents = "The quick brown fox jumps over the lazy dog";

  auto blob_client =
      blob_service_client_->GetBlobContainerClient(PreexistingContainerName())
          .GetBlockBlobClient(path_to_file);
  blob_client.UploadFrom(reinterpret_cast<const uint8_t*>(contents.data()),
                         contents.length());

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(path));
  EXPECT_EQ(fs_->io_context().external_id(), file->io_context().external_id());
}

TEST_F(AzuriteFileSystemTest, OpenInputFileInfo) {
  ASSERT_OK_AND_ASSIGN(auto info, fs_->GetFileInfo(PreexistingObjectPath()));

  std::shared_ptr<io::RandomAccessFile> file;
  ASSERT_OK_AND_ASSIGN(file, fs_->OpenInputFile(info));

  std::array<char, 1024> buffer{};
  std::int64_t size;
  auto constexpr kStart = 16;
  ASSERT_OK_AND_ASSIGN(size, file->ReadAt(kStart, buffer.size(), buffer.data()));

  auto const expected = std::string(kLoremIpsum).substr(kStart);
  EXPECT_EQ(std::string(buffer.data(), size), expected);
}

TEST_F(AzuriteFileSystemTest, OpenInputFileNotFound) {
  ASSERT_RAISES(IOError, fs_->OpenInputFile(NotFoundObjectPath()));
}

TEST_F(AzuriteFileSystemTest, OpenInputFileInfoInvalid) {
  ASSERT_OK_AND_ASSIGN(auto info, fs_->GetFileInfo(PreexistingContainerPath()));
  ASSERT_RAISES(IOError, fs_->OpenInputFile(info));

  ASSERT_OK_AND_ASSIGN(auto info2, fs_->GetFileInfo(NotFoundObjectPath()));
  ASSERT_RAISES(IOError, fs_->OpenInputFile(info2));
}

TEST_F(AzuriteFileSystemTest, OpenInputFileClosed) {
  ASSERT_OK_AND_ASSIGN(auto stream, fs_->OpenInputFile(PreexistingObjectPath()));
  ASSERT_OK(stream->Close());
  std::array<char, 16> buffer{};
  ASSERT_RAISES(Invalid, stream->Tell());
  ASSERT_RAISES(Invalid, stream->Read(buffer.size(), buffer.data()));
  ASSERT_RAISES(Invalid, stream->Read(buffer.size()));
  ASSERT_RAISES(Invalid, stream->ReadAt(1, buffer.size(), buffer.data()));
  ASSERT_RAISES(Invalid, stream->ReadAt(1, 1));
  ASSERT_RAISES(Invalid, stream->Seek(2));
}

}  // namespace
}  // namespace fs
}  // namespace arrow
