/*
* (C) 2006,2011,2012,2014,2015 Jack Lloyd
*
* Botan is released under the Simplified BSD License (see license.txt)
*/

#include "tests.h"

#if defined(BOTAN_HAS_X509_CERTIFICATES)
   #include <botan/x509path.h>
   #include <botan/calendar.h>
   #include <botan/internal/filesystem.h>
   #include <botan/parsing.h>
   #include <botan/data_src.h>
   #include <botan/x509_crl.h>
   #include <botan/pkcs10.h>
#endif

#include <botan/exceptn.h>

#include <fstream>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

namespace Botan_Tests {

namespace {

#if defined(BOTAN_HAS_X509_CERTIFICATES) && defined(BOTAN_HAS_RSA) && defined(BOTAN_HAS_EMSA_PKCS1) && defined(BOTAN_TARGET_OS_HAS_FILESYSTEM)

std::map<std::string, std::string> read_results(const std::string& results_file, const char delim = ':')
   {
   std::ifstream in(results_file);
   if(!in.good())
      {
      throw Test_Error("Failed reading " + results_file);
      }

   std::map<std::string, std::string> m;
   std::string line;
   while(in.good())
      {
      std::getline(in, line);
      if(line == "")
         {
         continue;
         }
      if(line[0] == '#')
         {
         continue;
         }

      std::vector<std::string> parts = Botan::split_on(line, delim);

      if(parts.size() != 2)
         {
         throw Test_Error("Invalid line " + line);
         }

      m[parts[0]] = parts[1];
      }

   return m;
   }

class X509test_Path_Validation_Tests final : public Test
   {
   public:
      std::vector<Test::Result> run() override
         {
         std::vector<Test::Result> results;

         // Test certs generated by https://github.com/yymax/x509test

         std::map<std::string, std::string> expected =
            read_results(Test::data_file("x509/x509test/expected.txt"));

         // Current tests use SHA-1
         const Botan::Path_Validation_Restrictions restrictions(false, 80);

         Botan::X509_Certificate root(Test::data_file("x509/x509test/root.pem"));
         Botan::Certificate_Store_In_Memory trusted;
         trusted.add_certificate(root);

         auto validation_time = Botan::calendar_point(2016, 10, 21, 4, 20, 0).to_std_timepoint();

         for(auto i = expected.begin(); i != expected.end(); ++i)
            {
            Test::Result result("X509test path validation");
            result.start_timer();
            const std::string filename = i->first;
            const std::string expected_result = i->second;

            std::vector<Botan::X509_Certificate> certs =
               load_cert_file(Test::data_file("x509/x509test/" + filename));

            if(certs.empty())
               {
               throw Test_Error("Failed to read certs from " + filename);
               }

            Botan::Path_Validation_Result path_result = Botan::x509_path_validate(
                     certs, restrictions, trusted,
                     "www.tls.test", Botan::Usage_Type::TLS_SERVER_AUTH,
                     validation_time);

            if(path_result.successful_validation() && path_result.trust_root() != root)
               {
               path_result = Botan::Path_Validation_Result(Botan::Certificate_Status_Code::CANNOT_ESTABLISH_TRUST);
               }

            result.test_eq("test " + filename, path_result.result_string(), expected_result);
            result.end_timer();
            results.push_back(result);
            }

         return results;
         }

   private:

      std::vector<Botan::X509_Certificate> load_cert_file(const std::string& filename)
         {
         Botan::DataSource_Stream in(filename);

         std::vector<Botan::X509_Certificate> certs;
         while(!in.end_of_data())
            {
            try
               {
               certs.emplace_back(in);
               }
            catch(Botan::Decoding_Error&) {}
            }

         return certs;
         }

   };

BOTAN_REGISTER_TEST("x509_path_x509test", X509test_Path_Validation_Tests);

class NIST_Path_Validation_Tests final : public Test
   {
   public:
      std::vector<Test::Result> run() override;
   };

std::vector<Test::Result> NIST_Path_Validation_Tests::run()
   {
   std::vector<Test::Result> results;

   /**
   * Code to run the X.509v3 processing tests described in "Conformance
   *  Testing of Relying Party Client Certificate Path Proccessing Logic",
   *  which is available on NIST's web site.
   *
   * Known Failures/Problems:
   *  - Policy extensions are not implemented, so we skip tests #34-#53.
   *  - Tests #75 and #76 are skipped as they make use of relatively
   *    obscure CRL extensions which are not supported.
   */
   const std::string nist_test_dir = Test::data_dir() + "/x509/nist";

   try
      {
      // Do nothing, just test filesystem access
      Botan::get_files_recursive(nist_test_dir);
      }
   catch(Botan::No_Filesystem_Access&)
      {
      Test::Result result("NIST path validation");
      result.test_note("Skipping due to missing filesystem access");
      results.push_back(result);
      return results;
      }

   std::map<std::string, std::string> expected =
      read_results(Test::data_file("x509/nist/expected.txt"));

   const Botan::X509_Certificate root_cert(nist_test_dir + "/root.crt");
   const Botan::X509_CRL root_crl(nist_test_dir + "/root.crl");

   for(auto i = expected.begin(); i != expected.end(); ++i)
      {
      Test::Result result("NIST path validation");
      result.start_timer();

      const std::string test_name = i->first;

      try
         {
         const std::string expected_result = i->second;

         const std::string test_dir = nist_test_dir + "/" + test_name;

         const std::vector<std::string> all_files = Botan::get_files_recursive(test_dir);

         if(all_files.empty())
            {
            result.test_failure("No test files found in " + test_dir);
            results.push_back(result);
            continue;
            }

         Botan::Certificate_Store_In_Memory store;

         store.add_certificate(root_cert);
         store.add_crl(root_crl);

         for(auto const& file : all_files)
            {
            if(file.find(".crt") != std::string::npos && file != "end.crt")
               {
               store.add_certificate(Botan::X509_Certificate(file));
               }
            else if(file.find(".crl") != std::string::npos)
               {
               Botan::DataSource_Stream in(file, true);
               Botan::X509_CRL crl(in);
               store.add_crl(crl);
               }
            }

         Botan::X509_Certificate end_user(test_dir + "/end.crt");

         // 1024 bit root cert
         Botan::Path_Validation_Restrictions restrictions(true, 80);

         Botan::Path_Validation_Result validation_result =
            Botan::x509_path_validate(end_user,
                                      restrictions,
                                      store);

         result.test_eq(test_name + " path validation result",
                        validation_result.result_string(),
                        expected_result);
         }
      catch(std::exception& e)
         {
         result.test_failure(test_name, e.what());
         }

      result.end_timer();
      results.push_back(result);
      }

   return results;
   }

BOTAN_REGISTER_TEST("x509_path_nist", NIST_Path_Validation_Tests);

class Extended_Path_Validation_Tests final : public Test
   {
   public:
      std::vector<Test::Result> run() override;
   };

std::vector<Test::Result> Extended_Path_Validation_Tests::run()
   {
   std::vector<Test::Result> results;

   const std::string extended_x509_test_dir = Test::data_dir() + "/x509/extended";

   try
      {
      // Do nothing, just test filesystem access
      Botan::get_files_recursive(extended_x509_test_dir);
      }
   catch(Botan::No_Filesystem_Access&)
      {
      Test::Result result("Extended x509 path validation");
      result.test_note("Skipping due to missing filesystem access");
      results.push_back(result);
      return results;
      }

   std::map<std::string, std::string> expected =
      read_results(Test::data_file("x509/extended/expected.txt"));

   auto validation_time = Botan::calendar_point(2017,9,1,9,30,33).to_std_timepoint();

   for(auto i = expected.begin(); i != expected.end(); ++i)
      {
      const std::string test_name = i->first;
      const std::string expected_result = i->second;

      const std::string test_dir = extended_x509_test_dir + "/" + test_name;

      Test::Result result("Extended X509 path validation");
      result.start_timer();

      const std::vector<std::string> all_files = Botan::get_files_recursive(test_dir);

      if(all_files.empty())
         {
         result.test_failure("No test files found in " + test_dir);
         results.push_back(result);
         continue;
         }

      Botan::Certificate_Store_In_Memory store;

      for(auto const& file : all_files)
         {
         if(file.find(".crt") != std::string::npos && file != "end.crt")
            {
            store.add_certificate(Botan::X509_Certificate(file));
            }
         }

      Botan::X509_Certificate end_user(test_dir + "/end.crt");

      Botan::Path_Validation_Restrictions restrictions;
      Botan::Path_Validation_Result validation_result =
         Botan::x509_path_validate(end_user,
                                   restrictions,
                                   store,
                                   "",
                                   Botan::Usage_Type::UNSPECIFIED,
                                   validation_time);

      result.test_eq(test_name + " path validation result",
                     validation_result.result_string(),
                     expected_result);

      result.end_timer();
      results.push_back(result);
      }

   return results;
   }

BOTAN_REGISTER_TEST("x509_path_extended", Extended_Path_Validation_Tests);

class PSS_Path_Validation_Tests : public Test
   {
   public:
      std::vector<Test::Result> run() override;
   };

std::vector<Test::Result> PSS_Path_Validation_Tests::run()
   {
   std::vector<Test::Result> results;

   const std::string pss_x509_test_dir = Test::data_dir() + "/x509/pss_certs";

   try
      {
      // Do nothing, just test filesystem access
      Botan::get_files_recursive(pss_x509_test_dir);
      }
   catch(Botan::No_Filesystem_Access&)
      {
      Test::Result result("RSA-PSS X509 signature validation");
      result.test_note("Skipping due to missing filesystem access");
      results.push_back(result);
      return results;
      }

   std::map<std::string, std::string> expected =
      read_results(Test::data_file("x509/pss_certs/expected.txt"));

   std::map<std::string, std::string> validation_times =
      read_results(Test::data_file("x509/pss_certs/validation_times.txt"));

   auto validation_times_iter = validation_times.begin();
   for(auto i = expected.begin(); i != expected.end(); ++i)
      {
      const std::string test_name = i->first;
      const std::string expected_result = i->second;

      const std::string test_dir = pss_x509_test_dir + "/" + test_name;

      Test::Result result("RSA-PSS X509 signature validation");
      result.start_timer();

      const std::vector<std::string> all_files = Botan::get_files_recursive(test_dir);

      if(all_files.empty())
         {
         result.test_failure("No test files found in " + test_dir);
         results.push_back(result);
         continue;
         }

      std::shared_ptr<Botan::X509_CRL> crl;
      std::shared_ptr<Botan::X509_Certificate> end;
      std::shared_ptr<Botan::X509_Certificate> root;
      Botan::Certificate_Store_In_Memory store;
      std::shared_ptr<Botan::PKCS10_Request> csr;
      auto validation_time = Botan::calendar_point(std::atoi((validation_times_iter++)->second.c_str()), 0, 0, 0, 0,
                             0).to_std_timepoint();
      for(auto const& file : all_files)
         {
         if(file.find("end.crt") != std::string::npos)
            {
            end.reset(new Botan::X509_Certificate(file));
            }
         else if(file.find("root.crt") != std::string::npos)
            {
            root.reset(new Botan::X509_Certificate(file));
            store.add_certificate(*root);
            }
         else if(file.find(".crl") != std::string::npos)
            {
            crl.reset(new Botan::X509_CRL(file));
            }
         else if(file.find(".csr") != std::string::npos)
            {
            csr.reset(new Botan::PKCS10_Request(file));
            }
         }

      if(end && crl && root)    // CRL tests
         {
         const std::vector<std::shared_ptr<const Botan::X509_Certificate>> cert_path = { end, root };
         const std::vector<std::shared_ptr<const Botan::X509_CRL>> crls = { crl };
         auto crl_status = Botan::PKIX::check_crl(cert_path, crls,
                           validation_time);   // alternatively we could just call crl.check_signature( root_pubkey )

         result.test_eq(test_name + " check_crl result",
                        Botan::Path_Validation_Result::status_string(Botan::PKIX::overall_status(crl_status)),
                        expected_result);
         }
      else if(end && root)     // CRT chain tests
         {
         // sha-1 is used
         Botan::Path_Validation_Restrictions restrictions(false, 80);

         Botan::Path_Validation_Result validation_result =
            Botan::x509_path_validate(*end,
                                      restrictions,
                                      store, "", Botan::Usage_Type::UNSPECIFIED, validation_time);

         result.test_eq(test_name + " path validation result",
                        validation_result.result_string(),
                        expected_result);
         }
      else if(end && !root)    // CRT self signed tests
         {
         std::unique_ptr<Botan::Public_Key> pubkey(end->subject_public_key());
         result.test_eq(test_name + " verify signature", end->check_signature(*pubkey), !!(std::stoi(expected_result)));
         }
      else if(csr)    // PKCS#10 Request
         {
         std::unique_ptr<Botan::Public_Key> pubkey(csr->subject_public_key());
         result.test_eq(test_name + " verify signature", csr->check_signature(*pubkey), !!(std::stoi(expected_result)));
         }

      result.end_timer();
      results.push_back(result);
      }

   return results;
   }

BOTAN_REGISTER_TEST("x509_path_rsa_pss", PSS_Path_Validation_Tests);

class BSI_Path_Validation_Tests final : public Test
   {
   public:
          std::vector<Test::Result> run() override;
   };

std::vector<Test::Result> BSI_Path_Validation_Tests::run()
   {
   std::vector<Test::Result> results;

   const std::string bsi_test_dir = Test::data_dir() + "/x509/bsi";

   try
      {
      // Do nothing, just test filesystem access
      Botan::get_files_recursive(bsi_test_dir);
      }
   catch (Botan::No_Filesystem_Access&)
      {
      Test::Result result("BSI path validation");
      result.test_note("Skipping due to missing filesystem access");
      results.push_back(result);
      return results;
      }

   std::map<std::string, std::string> expected = read_results(
         Test::data_file("/x509/bsi/expected.txt"), '$');

   for (auto& i : expected)
      {
      const std::string test_name = i.first;
      const std::string expected_result = i.second;

      const std::string test_dir = bsi_test_dir + "/" + test_name;

      Test::Result result("BSI path validation");
      result.start_timer();

      const std::vector<std::string> all_files =
            Botan::get_files_recursive(test_dir);

      if (all_files.empty())
         {
         result.test_failure("No test files found in " + test_dir);
         results.push_back(result);
         continue;
         }

      Botan::Certificate_Store_In_Memory trusted;
      std::vector<Botan::X509_Certificate> certs;

      auto validation_time =
            Botan::calendar_point(2017, 8, 19, 12, 0, 0).to_std_timepoint();

      // By convention: if CRL is a substring if the directory name,
      // we need to check the CRLs
      bool use_crl = false;
      if (test_dir.find("CRL") != std::string::npos)
         {
         use_crl = true;
         }

      try
         {
         for (auto const& file : all_files)
            {
            // found a trust anchor
            if (file.find("TA") != std::string::npos)
               {
               trusted.add_certificate(Botan::X509_Certificate(file));
               }
            // found the target certificate. It needs to be at the front of certs
            else if (file.find("TC") != std::string::npos)
               {
               certs.insert(certs.begin(), Botan::X509_Certificate(file));
               }
            // found a certificate that might be part of a valid certificate chain to the trust anchor
            else if (file.find(".crt") != std::string::npos)
               {
               certs.push_back(Botan::X509_Certificate(file));
               }
            else if (file.find(".crl") != std::string::npos)
               {
               trusted.add_crl(Botan::X509_CRL(file));
               }
            }

         Botan::Path_Validation_Restrictions restrictions(use_crl, 79,
               use_crl);

         /*
          * Following the test document, the test are executed 16 times with
          * randomly chosen order of the available certificates. However, the target
          * certificate needs to stay in front.
          * For certain test, the order in which the certificates are given to
          * the validation function may be relevant, i.e. if issuer DNs are
          * ambiguous.
          */
         auto uniform_shuffle = [](size_t m) -> size_t
         {
            size_t s;
            Test::rng().randomize(reinterpret_cast<uint8_t*>(&s), sizeof(s));
            return s % m;
         };

         for (size_t r = 0; r < 16; r++)
            {
            std::random_shuffle(++(certs.begin()), certs.end(), uniform_shuffle);

            Botan::Path_Validation_Result validation_result =
                  Botan::x509_path_validate(certs, restrictions, trusted, "",
                        Botan::Usage_Type::UNSPECIFIED, validation_time);

            // We expect to be warned
            if(expected_result.find("Warning: ") == 0)
               {
               std::string stripped = expected_result.substr(std::string("Warning: ").size());
               bool found_warning = false;
               for(const auto& warning_set : validation_result.warnings())
                  {
                  for(const auto& warning : warning_set)
                     {
                     std::string warning_str(Botan::to_string(warning));
                     if(stripped == warning_str)
                        {
                        result.test_eq(test_name + " path validation result",
                              warning_str, stripped);
                        found_warning = true;
                        }
                     }
                  }
               if(!found_warning)
                  {
                  result.test_failure(test_name,"Did not receive the expected warning: " + stripped);
                  }
               }
            else
               {
               result.test_eq(test_name + " path validation result",
                     validation_result.result_string(), expected_result);
               }


            }
         }

      /* Some certificates are rejected when executing the X509_Certificate constructor
       * by throwing a Decoding_Error exception.
       */
      catch (const Botan::Decoding_Error& d)
         {
         result.test_eq(test_name + " path validation result", d.what(),
               expected_result);
         }
      catch (const Botan::X509_CRL::X509_CRL_Error& e)
         {
         result.test_eq(test_name + " path validation result", e.what(),
               expected_result);
         }
      catch (const std::exception& e)
         {
         result.test_failure(test_name, e.what());
         }

      result.end_timer();
      results.push_back(result);
      }

   return results;
   }

BOTAN_REGISTER_TEST("x509_path_bsi", BSI_Path_Validation_Tests);

#endif

}

}
