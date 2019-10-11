#include "util/fake_ofstream.hh"
#include "util/file_piece.hh"
#include "util/file.hh"
#include "util/murmur_hash.hh"
#include "util/pcqueue.hh"
#include "util/pool.hh"

#include <string>
#include <thread>
#include <unordered_map>
#include <limits>

#include <signal.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <boost/program_options/options_description.hpp>
#include <boost/program_options/parsers.hpp>
#include <boost/program_options/variables_map.hpp>

struct Options {
  std::string key;
  std::string field_separator;
};

void Pipe(util::scoped_fd &first, util::scoped_fd &second) {
  int fds[2];
  UTIL_THROW_IF(pipe(fds), util::ErrnoException, "Creating pipe failed");
  first.reset(fds[0]);
  second.reset(fds[1]);
}

pid_t Launch(char *argv[], util::scoped_fd &in, util::scoped_fd &out) {
  util::scoped_fd process_in, process_out;
  Pipe(process_in, in);
  Pipe(out, process_out);
  pid_t pid = fork();
  UTIL_THROW_IF(pid == -1, util::ErrnoException, "Fork failed");
  if (pid == 0) {
    // Inside child process.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    UTIL_THROW_IF(-1 == dup2(process_in.get(), STDIN_FILENO), util::ErrnoException, "dup2 failed for process stdin from " << process_in.get());
    UTIL_THROW_IF(-1 == dup2(process_out.get(), STDOUT_FILENO), util::ErrnoException, "dup2 failed for process stdout from " << process_out.get());
    in.reset();
    out.reset();
    execvp(argv[0], argv);
    util::ErrnoException e;
    std::cerr << "exec " << argv[0] << " failed: " << e.what() << std::endl;
    abort();
  }
  // Parent closes parts it doesn't need in destructors.
  return pid;
}

struct QueueEntry {
  // NULL pointer is poison.
  // value->data() == NULL means uninitialized
  StringPiece *value;
};

void Input(util::UnboundedSingleQueue<QueueEntry> &queue, util::scoped_fd &process_input, std::unordered_map<uint64_t, StringPiece> &cache, std::size_t flush_rate, Options &options) {
  QueueEntry q_entry;
  {
    util::FakeOFStream process(process_input.get());
    std::pair<uint64_t, StringPiece> entry;
    std::size_t flush_count = flush_rate;
    // Parse column numbers, if given using --key option, into an integer vector (comma separated integers)
    std::vector<int> columns_args;

    size_t pos_args = 0;
    std::string token_args;
    std::string s_args = options.key;
    while ((pos_args = s_args.find(",")) != std::string::npos) {
      token_args = s_args.substr(0, pos_args);
      std::string::size_type sz;
      int int_token_args = std::stoi (token_args, &sz);
      columns_args.push_back(int_token_args);
      s_args.erase(0, pos_args + 1);
    }
    std::string::size_type sz;
    int last_int_token_args = std::stoi (s_args, &sz);
    columns_args.push_back(last_int_token_args);

    for (StringPiece l : util::FilePiece(STDIN_FILENO)) {
      // Split content columns, if given using --key option, into a string vector
      std::vector<std::string> columns;
 
      size_t pos = 0;
      std::string token;
      std::string s (l.data(), l.size());
      while ((pos = s.find(options.field_separator)) != std::string::npos) {
        token = s.substr(0, pos);
        columns.push_back(token);
        s.erase(0, pos + options.field_separator.length());
      }
      columns.push_back(s);
      // Get max/min argument --key to manage some errors
      int min = std::numeric_limits<int>::max();
      int max = 0;
      for (int i : columns_args){
        if (i < min){
          min = i;
        }
        if (i > max){
          max = i;
        }
      }
      // If any column number is out of boundaries (or --key is not provided, so uses default -1), use the whole line as data for mmh
      if (columns.size() < max || min <= 0){
        entry.first = util::MurmurHashNative(l.data(), l.size());
      }
      else{
        std::string hash_string;
        for (int i : columns_args){
          hash_string += columns.at(i-1);
        }
        entry.first = util::MurmurHashNative(hash_string.c_str(), hash_string.size());
      }

      std::pair<std::unordered_map<uint64_t, StringPiece>::iterator, bool> res(cache.insert(entry));
      if (res.second) {
        // New entry.  Send to captive process.
        process << l << '\n';
        // Guarantee we flush to process every so often.
        if (!--flush_count) {
          process.Flush();
          flush_count = flush_rate;
        }
      }
      // Pointer to hash table entry.
      q_entry.value = &res.first->second;
      // Deadlock here if the captive program buffers too many lines.
      queue.Produce(q_entry);
    }
  }
  process_input.reset();
  // Poison.
  q_entry.value = NULL;
  queue.Produce(q_entry);
}

// Read from queue.  If it's not in the cache, read the result from the captive
// process.
void Output(util::UnboundedSingleQueue<QueueEntry> &queue, util::scoped_fd &process_output) {
  util::FakeOFStream out(STDOUT_FILENO);
  util::FilePiece in(process_output.release());
  // We'll allocate the cached strings into a pool.
  util::Pool string_pool;
  QueueEntry q;
  while (queue.Consume(q).value) {
    StringPiece &value = *q.value;
    if (!value.data()) {
      // New entry, not cached.
      StringPiece got = in.ReadLine();
      // Allocate memory to store a copy of the line.
      char *copy_to = (char*)string_pool.Allocate(got.size());
      memcpy(copy_to, got.data(), got.size());
      value = StringPiece(copy_to, got.size());
    }
    out << value << '\n';
  }
}

// Parse arguments using boost::program_options
void ParseArgs(int argc, char *argv[], Options &out) {
  namespace po = boost::program_options;
  po::options_description desc("Acts as a cache around another program processing one line in, one line out from stdin to stdout.");
  desc.add_options()
    ("key,k", po::value(&out.key)->default_value("-1"), "Column(s) key to use as the deduplication string")
    ("field_separator,t", po::value(&out.field_separator)->default_value("\t"), "use a field separator instead of tab");
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, desc), vm);
  po::notify(vm);
}

int main(int argc, char *argv[]) {
  // The underlying program can buffer up to kQueueLength - kFlushRate lines.  If it goes above that, deadlock waiting for queue.
  const std::size_t kFlushRate = 4096;
  
  // Take into account the number of arguments given to `cache` to delete them from the argv provided to Launch function
  Options opt;
  int skip_args = 1;
  std::string s1(argv[1]);
  if (s1 == "-k" || s1 == "-t" || s1 == "--key" || s1 == "--field_separator"){
    skip_args += 2;
  }
  if (argc > 3){
    std::string s3(argv[3]);
    if (s3 == "-k" || s3 == "-t" || s3 == "--key" || s3 == "--field_separator"){
      skip_args += 2;
    }
  }
  char *partArgs[skip_args];
  std::copy(argv, &argv[skip_args+1], partArgs);
  ParseArgs(skip_args, partArgs, opt);

  util::scoped_fd in, out;
  pid_t child = Launch(argv + skip_args, in, out);
  // We'll deadlock if this queue is full and the program is buffering.
  util::UnboundedSingleQueue<QueueEntry> queue;
  // This cache has to be alive for Input and Output because Input passes pointers through the queue.
  std::unordered_map<uint64_t, StringPiece> cache;
  // Run Input and Output concurrently.  Arbitrarily, we'll do Output in the main thread.
  std::thread input([&queue, &in, &cache, kFlushRate, &opt]{Input(queue, in, cache, kFlushRate, opt);});
  Output(queue, out);
  input.join();
  int status;
  UTIL_THROW_IF(-1 == waitpid(child, &status, 0), util::ErrnoException, "waitpid for child failed");
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  } else {
    return 256;
  }
}
