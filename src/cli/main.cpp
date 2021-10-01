#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <yaml-cpp/yaml.h>
#include "mirror.hpp"
#include "mirrors/oci.hpp"
#include "mirrors/s3.hpp"
#include "url.hpp"
#include "utils.hpp"
#include "downloader.hpp"

using namespace powerloader;

enum KindOf
{
    kHTTP,
    kOCI,
    kS3
};

struct
{
    std::size_t done;
    std::size_t total;

    std::map<DownloadTarget*, curl_off_t> total_done;
} global_progress;

struct MirrorCredentials
{
    URLHandler url;
    std::string user, password, region;
};


int
progress_callback(DownloadTarget* t, curl_off_t total, curl_off_t done)
{
    if (total == 0 || done == 0)
        return 0;
    if (global_progress.total_done.size() != 0)
        std::cout << "\x1b[1A\r";
    if (global_progress.total_done.find(t) == global_progress.total_done.end())
    {
        if (!total)
            return 0;
        global_progress.total_done[t] = done;
        global_progress.total += total;
    }
    else
    {
        global_progress.total_done[t] = done;
    }

    double total_done = 0;
    for (auto& [k, v] : global_progress.total_done)
        total_done += v;
    total_done /= global_progress.total;

    std::size_t bar_width = 50;
    std::cout << "[";
    int pos = bar_width * total_done;
    for (int i = 0; i < bar_width; ++i)
    {
        if (i < pos)
            std::cout << "=";
        else if (i == pos)
            std::cout << ">";
        else
            std::cout << " ";
    }
    std::cout << "] " << int(total_done * 100.0) << " %\n";
    std::cout.flush();

    return 0;
}

int
handle_upload(const std::vector<std::string>& files, const std::vector<std::string>& mirrors)
{
    std::string mirror_url = mirrors[0];
    if (mirrors.size() > 1)
        spdlog::warn("Only uploading to first mirror");

    KindOf kof = KindOf::kHTTP;
    std::unique_ptr<Mirror> mptr;

    URLHandler url(mirror_url);

    if (url.scheme() == "s3")
        kof = KindOf::kS3;
    else if (url.scheme() == "oci")
        kof = KindOf::kOCI;

    if (kof != KindOf::kHTTP)
        url.set_scheme("https");

    spdlog::info("URL: {}", url.url());

    for (auto& f : files)
    {
        auto elems = split(f, ":");
        fs::path fpath = elems[0];
        std::string dest = elems[1];

        if (kof == KindOf::kOCI)
        {
            if (elems.size() != 3)
            {
                spdlog::error("For OCI upload we need file:destname:tag");
                return 1;
            }
            std::string GH_SECRET = get_env("GHA_PAT");
            std::string GH_USER = get_env("GHA_USER");

            OCIMirror mirror(url.url(), "push", GH_USER, GH_SECRET);
            oci_upload(mirror, GH_USER + "/" + dest, elems[2], elems[0]);
        }
        else if (kof == KindOf::kS3)
        {
            if (elems.size() != 2)
            {
                spdlog::error("For S3 upload we need file:destpath");
                return 1;
            }

            std::string aws_ackey = get_env("AWS_ACCESS_KEY");
            std::string aws_sekey = get_env("AWS_SECRET_KEY");
            std::string aws_region = get_env("AWS_DEFAULT_REGION");

            std::string url_ = url.url();
            if (url_.back() == '/')
                url_ = url_.substr(0, url_.size() - 1);

            S3Mirror s3mirror(url_, aws_region, aws_ackey, aws_sekey);
            s3_upload(s3mirror, elems[1], elems[0]);
        }
    }

    return 1;
}

int
handle_download(const std::vector<std::string>& urls,
                const std::vector<std::string>& mirrors,
                bool resume,
                const std::string& outfile,
                const std::string& sha_cli,
                const std::string& dest_folder,
                long int filesize)
{
    // the format for URLs is:
    // conda-forge:linux-64/xtensor-123.tar.bz2[:xtensor.tar.bz2] (last part optional, can be
    // inferred from `path`)
    // https://conda.anaconda.org/conda-forge/linux-64/xtensor-123.tar.bz2[:xtensor.tar.bz2]
    std::vector<std::unique_ptr<DownloadTarget>> targets;

    auto& ctx = Context::instance();

    for (auto& x : urls)
    {
        if (contains(x, "://"))
        {
            // even when we get a regular URL like `http://test.com/download.tar.gz`
            // we want to create a "mirror" for `http://test.com` to make sure we correctly
            // retry and wait on mirror failures
            URLHandler uh(x);
            std::string url = uh.url();
            std::string host = uh.host();
            std::string path = uh.path();
            std::string mirror_url = url.substr(0, url.size() - path.size());
            std::string dst = outfile.empty() ? rsplit(uh.path(), "/", 1).back() : outfile;

            if (ctx.mirror_map.find(host) == ctx.mirror_map.end())
            {
                ctx.mirror_map[host] = std::make_shared<std::vector<Mirror*>>();
            }

            ctx.mirrors.emplace_back(new Mirror(mirror_url));
            ctx.mirror_map[host]->push_back(ctx.mirrors.back().get());
            targets.emplace_back(new DownloadTarget(path.substr(1, std::string::npos), host, dst));
        }
        else
        {
            std::vector<std::string> parts = split(x, ":");
            std::string path, mirror;
            if (parts.size() == 2)
            {
                mirror = parts[0];
                path = parts[1];
            }
            else
            {
                throw std::runtime_error("Not the correct number of : in the url");
            }
            std::string dst = outfile.empty() ? rsplit(path, "/", 1).back() : outfile;

            if (!dest_folder.empty())
                dst = dest_folder + "/" + dst;

            spdlog::info("Downloading {} from {} to {}", path, mirror, dst);
            targets.emplace_back(new DownloadTarget(path, mirror, dst));
        }
        targets.back()->resume = resume;

        if (!sha_cli.empty())
            targets.back()->checksums.push_back(Checksum{ ChecksumType::kSHA256, sha_cli });
        if (filesize > 0)
            targets.back()->expected_size = filesize;

        using namespace std::placeholders;
        targets.back()->progress_callback
            = std::bind(&progress_callback, targets.back().get(), _1, _2);
    }

    Downloader dl;
    dl.mirror_map = ctx.mirror_map;

    for (auto& t : targets)
    {
        dl.add(t.get());
    }

    bool success = dl.download();
    if (!success)
    {
        spdlog::error("Download was not successful");
        exit(1);
    }

    return 0;
}

std::map<std::string, std::vector<std::unique_ptr<Mirror>>> parse_mirrors(const YAML::Node& node)
{
    assert(node.IsMap());
    std::map<std::string, std::vector<std::unique_ptr<Mirror>>> res;

    auto get_env_from_str = [](const std::string& s)
    {
        if (starts_with(s, "env:"))
        {
            return get_env(s.substr(4).c_str());
        }
        return s;
    };

    for (YAML::Node::const_iterator outer = node.begin(); outer != node.end(); ++outer)
    {
        std::string mirror_name = outer->first.as<std::string>();
        res[mirror_name] = std::vector<std::unique_ptr<Mirror>>();

        for (YAML::Node::const_iterator it = outer->begin(); it != outer->end(); ++it)
        {
            MirrorCredentials creds;
            if (it->IsSequence())
            {
                creds.url = it->as<std::string>();
            }
            else
            {
                // expecting a map
                auto cred = *it;
                creds.url = URLHandler(cred["url"].as<std::string>());
                if (cred["password"])
                {
                    creds.password = get_env_from_str(cred["password"].as<std::string>());
                }
                if (cred["user"])
                {
                    creds.user = get_env_from_str(cred["user"].as<std::string>());
                }
                if (cred["region"])
                {
                    creds.region = get_env_from_str(cred["region"].as<std::string>());
                }                
            }
            auto kof = KindOf::HTTP;
            if (creds.url.scheme() == "s3")
            {
                kof = KindOf::S3;

                if (creds.user.empty()) creds.user = get_env("AWS_ACCESS_KEY");
                if (creds.password.empty()) creds.password = get_env("AWS_SECRET_KEY");
                if (creds.region.empty()) creds.region = get_env("AWS_DEFAULT_REGION");
            }
            else if (creds.url.scheme() == "oci")
            {
                kof = KindOf::OCI;
                if (creds.user.empty()) creds.user = get_env("GHA_USER");
                if (creds.password.empty()) creds.password = get_env("GHA_PAT");
            }

            if (kof != KindOf::HTTP)
                creds.url.set_scheme("https");

            if (kof == KindOf::S3)
            {
                res[mirror_name].emplace_back(new S3Mirror(creds.url.url(), creds.region, creds.user, creds.password));
            }
            else if (kof == KindOf::OCI)
            {
                res[mirror_name].emplace_back(new OCIMirror(creds.url.url(), "push,pull", creds.user, creds.password));
            }
            else if (kof == KindOf::HTTP)
            {
                res[mirror_name].emplace_back(new Mirror(creds.url.url()));
            }
        }
    }
    return res;
}


int
main(int argc, char** argv)
{
    CLI::App app;

    bool resume = false;
    std::vector<std::string> du_files;
    std::vector<std::string> mirrors;
    std::string file, outfile, sha_cli, outdir;
    bool verbose = false;
    long int filesize = -1;

    CLI::App* s_dl = app.add_subcommand("download", "Download a file");
    s_dl->add_option("files", du_files, "Files to download");
    s_dl->add_option("-m", mirrors, "Mirrors from where to download");
    s_dl->add_flag("-r,--resume", resume, "Try to resume");
    s_dl->add_option("-f", file, "File from which to read upload / download files");
    s_dl->add_option("-o", outfile, "Output file");
    s_dl->add_option("-d", outdir, "Output directory");

    CLI::App* s_ul = app.add_subcommand("upload", "Upload a file");
    s_ul->add_option("files", du_files, "Files to upload");
    s_ul->add_option("-m", mirrors, "Mirror to upload to");
    s_ul->add_option("-f", file, "File from which to read mirrors, upload & download files");

    s_ul->add_flag("-v", verbose, "Enable verbose output");
    s_dl->add_flag("-v", verbose, "Enable verbose output");

    s_dl->add_option("--sha", sha_cli, "Expected SHA string");
    s_dl->add_option("-i", filesize, "Expected file size");

    CLI11_PARSE(app, argc, argv);

    if (verbose)
        Context::instance().set_verbosity(1);

    std::vector<Mirror> mlist;
    if (!file.empty())
    {
        YAML::Node config = YAML::LoadFile(file);

        auto& ctx = Context::instance();

        du_files = config["targets"].as<std::vector<std::string>>();
        if (config["mirrors"])
        {
            // ctx.mirror_map = parse_mirrors(config["mirrors"]);
            // for(auto it = config["mirrors"].begin(); it != config["mirrors"].end(); ++it)
            // {

            //    std::string key = it->first.as<std::string>();       // <- key
            //    cTypeList.push_back(it->second.as<CharacterType>()); // <- value
            // }

        //     for (const auto& [k, v] :
        //          config["mirrors"].as<std::map<std::string, std::vector<std::string>>>())
        //     {
        //         ctx.mirror_map[k] = std::make_shared<std::vector<Mirror*>>();
        //         for (auto& m : v)
        //         {
        //             std::cout << fmt::format("Adding mirror {} for {}", k, m) << std::endl;

        //             if (starts_with(m, "oci://"))
        //             {
        //                 try
        //                 {

        //                     std::string GH_SECRET = get_env("GHA_PAT");
        //                     std::string GH_USER = get_env("GHA_USER");
        //                     ctx.mirrors.emplace_back(new OCIMirror(m, "pull", GH_USER, GH_SECRET));
        //                 }
        //                 catch (...)
        //                 {
        //                     ctx.mirrors.emplace_back(new OCIMirror(m, "pull", "", ""));
        //                 }
        //             }
        //             else if (starts_with(m, "s3://"))
        //             {
        //                 std::string aws_ackey = get_env("AWS_ACCESS_KEY");
        //                 std::string aws_sekey = get_env("AWS_SECRET_KEY");
        //                 std::string aws_region = get_env("AWS_DEFAULT_REGION");
        //                 ctx.mirrors.emplace_back(new S3Mirror(m, aws_region, aws_ackey, aws_sekey));
        //             }
        //             else
        //             {
        //                 ctx.mirrors.emplace_back(new Mirror(m));
        //             }

        //             ctx.mirror_map[k]->push_back(ctx.mirrors.back().get());
        //         }
        //     }
        }
    }
    spdlog::set_level(spdlog::level::warn);
    if (app.got_subcommand("upload"))
    {
        return handle_upload(du_files, mirrors);
    }
    if (app.got_subcommand("download"))
    {
        return handle_download(du_files, mirrors, resume, outfile, sha_cli, outdir, filesize);
    }

    return 0;
}
