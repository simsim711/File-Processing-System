#include <algorithm>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <string>
#include <sys/resource.h> 
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <iomanip>

// Define constants
const int MAX_THREADS = 4;

// Mutex to protect access to the shared data structure (word count map)
std::mutex word_count_mutex;

// Struct for passing additional arguments to threads
struct ThreadData {
  std::string *text_part;
  std::unordered_map<std::string, int> *word_count_map;
};

// Function to count word frequencies in a portion of the file (multi-threaded)
void *count_words(void *arg) {
  auto *data = (ThreadData *)arg;
  std::string *text_part = data->text_part;
  auto *word_count_map = data->word_count_map;
  std::string word;

  std::unordered_map<std::string, int> local_word_count;  // Local map for each thread

  for (size_t i = 0; i < text_part->length(); i++) {
    char c = (*text_part)[i];
    if (isalpha(c)) {
      word += tolower(c); // Accumulate characters to form a word
    } else if (!word.empty()) {
      local_word_count[word]++;
      word.clear(); // Reset word after storing
    }
  }

  if (!word.empty()) { // Add the last word if it exists
    local_word_count[word]++;
  }

  // Update the shared word count map with thread-local results
  std::lock_guard<std::mutex> lock(word_count_mutex);  // Lock to prevent data race
  for (const auto &pair : local_word_count) {
    (*word_count_map)[pair.first] += pair.second;
  }

  return nullptr;
}

// Single-threaded version for comparison
std::unordered_map<std::string, int> process_file_single_thread(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Error opening file: " << filename << std::endl;
    return {};
  }

  std::string file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::unordered_map<std::string, int> word_count_map;
  std::string word;
  for (size_t i = 0; i < file_content.length(); i++) {
    char c = file_content[i];
    if (isalpha(c)) {
      word += tolower(c); // Accumulate characters to form a word
    } else if (!word.empty()) {
      word_count_map[word]++;
      word.clear(); // Reset word after storing
    }
  }
  return word_count_map;
}

// Multi-threaded version
std::unordered_map<std::string, int> process_file_multi_thread(const std::string &filename) {
  std::ifstream file(filename);
  if (!file.is_open()) {
    std::cerr << "Error opening file: " << filename << std::endl;
    return {};
  }

  std::string file_content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  std::vector<std::string> parts(MAX_THREADS);

  for (int i = 0; i < MAX_THREADS; i++) {
    parts[i] = file_content.substr(i * file_content.length() / MAX_THREADS,
                                   file_content.length() / MAX_THREADS);
  }

  pthread_t threads[MAX_THREADS];
  std::unordered_map<std::string, int> total_word_count;
  ThreadData thread_data[MAX_THREADS];

  for (int i = 0; i < MAX_THREADS; i++) {
    thread_data[i] = {&parts[i], &total_word_count};
    int thread_result = pthread_create(&threads[i], NULL, count_words, (void *)&thread_data[i]);
    if (thread_result != 0) {
      std::cerr << "Error creating thread: " << thread_result << std::endl;
      exit(1);
    }
  }

  for (int i = 0; i < MAX_THREADS; i++) {
    pthread_join(threads[i], nullptr);
  }

  return total_word_count;
}

// Function to extract the top N most frequent words
std::vector<std::pair<std::string, int>> get_top_frequent_words(const std::unordered_map<std::string, int> &word_count_map, int top_n = 10) {
  std::vector<std::pair<std::string, int>> word_freqs(word_count_map.begin(), word_count_map.end());

  // Sort by frequency in descending order
  std::sort(word_freqs.begin(), word_freqs.end(), [](const auto &a, const auto &b) {
    return a.second > b.second;
  });

  if (word_freqs.size() > top_n) {
    word_freqs.resize(top_n); // Keep only the top N words
  }

  return word_freqs;
}

// Function to compare single-threaded vs multi-threaded performance
void compare_performance(const std::vector<std::string> &files) {
  for (const std::string &file : files) {
    std::cout << "Processing file: " << file << "\n";

    // Single-threaded
    auto start_single = std::chrono::high_resolution_clock::now();
    auto word_count_single = process_file_single_thread(file);
    auto end_single = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_single = end_single - start_single;
    std::cout << "  Single-threaded time: " << elapsed_single.count() << " seconds\n";

    // Multi-threaded
    auto start_multi = std::chrono::high_resolution_clock::now();
    auto word_count_multi = process_file_multi_thread(file);
    auto end_multi = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_multi = end_multi - start_multi;
    std::cout << "  Multi-threaded time:  " << elapsed_multi.count() << " seconds\n";

    // Check results (word counts should match)
    if (word_count_single == word_count_multi) {
      std::cout << "  Results match for file: " << file << "\n";
    } else {
      std::cout << "  Results mismatch for file: " << file << "\n";
    }
  }
}

// Function to process files using multiprocessing (fork) and print file name with word count
void process_files_with_fork(const std::vector<std::string> &files) {
  int fd[2];
  if (pipe(fd) == -1) {
    std::cerr << "Error creating pipe" << std::endl;
    exit(1);
  }

  for (const std::string &file : files) {
    pid_t pid = fork();
    if (pid == -1) {
      std::cerr << "Error in fork" << std::endl;
      exit(1);
    }

    if (pid == 0) { // Child process
      close(fd[0]); // Close reading end
      std::unordered_map<std::string, int> word_count = process_file_multi_thread(file);
      int count = word_count.size();

      // Send count to parent process
      if (write(fd[1], &count, sizeof(count)) == -1) {
        std::cerr << "Error writing to pipe" << std::endl;
        exit(1);
      }

      // Display the most frequent words for the file
      std::vector<std::pair<std::string, int>> top_words = get_top_frequent_words(word_count);
      std::cout << "\n  Most frequent words in file " << file << ":\n";
      for (const auto &pair : top_words) {
        std::cout << "    " << std::left << std::setw(15) << pair.first << ": " << pair.second << "\n";
      }

      close(fd[1]);
      exit(0);
    }
  }

  close(fd[1]); // Close writing end
  int total_count = 0, word_count;

  for (size_t i = 0; i < files.size(); i++) {
    if (read(fd[0], &word_count, sizeof(word_count)) == -1) {
      std::cerr << "Error reading from pipe" << std::endl;
      exit(1);
    }
    // Print the file name and word count for each file
    std::cout << "\n  Word count in file: " << files[i] << ": " << word_count << "\n";
    total_count += word_count;

    // Wait for child processes to finish
    wait(nullptr);
  }
  close(fd[0]);

  std::cout << "\nTotal word count across all files: " << total_count << std::endl;
}

// Function to measure and print resource usage (CPU time, memory usage)
void print_resource_usage() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    std::cout << "\nResource Usage:\n";
    std::cout << "  CPU time used (user):    " << usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 << " seconds\n";
    std::cout << "  CPU time used (system):  " << usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6 << " seconds\n";
    std::cout << "  Maximum memory usage:    " << usage.ru_maxrss << " kilobytes\n";
  } else {
    std::cerr << "Error retrieving resource usage data\n";
  }
}

int main() {
  // List of files to process
  std::vector<std::string> files = {
      "calgary/bib",   "calgary/paper1", "calgary/paper2", "calgary/progc",
      "calgary/progl", "calgary/progp",  "calgary/trans"};

  // Compare single-threaded vs multi-threaded performance
  compare_performance(files);

  // Measure time for multiprocessing + multithreading
  auto start = std::chrono::high_resolution_clock::now();
  process_files_with_fork(files);
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_seconds = end - start;

  std::cout << "\nElapsed time for multiprocessing + multithreading: " << elapsed_seconds.count() << " seconds\n";

  // Print resource usage (CPU and memory)
  print_resource_usage();

  return 0;
}
