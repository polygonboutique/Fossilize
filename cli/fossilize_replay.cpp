/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "volk.h"
#include "device.hpp"
#include "fossilize.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "file.hpp"
#include "path.hpp"
#include "fossilize_db.hpp"

#include <cinttypes>
#include <string>
#include <unordered_set>
#include <stdlib.h>
#include <string.h>
#include <chrono>	// VALVE
#include <queue>	// VALVE
#include <thread>	// VALVE
#include <mutex>	// VALVE
#include <condition_variable> // VALVE
#include <fstream>
#include <atomic>

using namespace Fossilize;
using namespace std;

struct ThreadedReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;
		string on_disk_pipeline_cache_path;

		// VALVE: Add multi-threaded pipeline creation
		unsigned num_threads = thread::hardware_concurrency();

		// VALVE: --loop option for testing performance
		unsigned loop_count = 1;
	};

	ThreadedReplayer(const VulkanDevice::Options &device_opts_, const Options &opts_,
	             const unordered_set<Hash> &graphics,
	             const unordered_set<Hash> &compute)
		: opts(opts_), filter_graphics(graphics), filter_compute(compute),
		  num_worker_threads(opts.num_threads ), loop_count(opts.loop_count),
		  device_opts(device_opts_)
	{
		// Cannot use initializers for atomics.
		graphics_pipeline_ns.store(0);
		compute_pipeline_ns.store(0);
		shader_module_ns.store(0);
		graphics_pipeline_count.store(0);
		compute_pipeline_count.store(0);
		shader_module_count.store(0);

		// Create a thread pool with the # of specified worker threads (defaults to thread::hardware_concurrency()).
		for (unsigned i = 0; i < num_worker_threads; i++)
			thread_pool.push_back(std::thread(&ThreadedReplayer::worker_thread, this));
	}

	void sync_worker_threads()
	{
		unique_lock<mutex> lock(pipeline_work_queue_mutex);
		work_done_condition.wait(lock, [&]() -> bool {
			return queued_count == completed_count;
		});
	}

	void worker_thread()
	{
		uint64_t graphics_ns = 0;
		unsigned graphics_count = 0;

		uint64_t compute_ns = 0;
		unsigned compute_count = 0;

		uint64_t shader_ns = 0;
		unsigned shader_count = 0;

		for (;;)
		{
			PipelineWorkItem work_item;
			{
				unique_lock<mutex> lock(pipeline_work_queue_mutex);
				work_available_condition.wait(lock, [&]() -> bool {
					return shutting_down || !pipeline_work_queue.empty();
				});

				if (shutting_down)
					break;

				work_item = pipeline_work_queue.front();
				pipeline_work_queue.pop();
			}

			switch (work_item.tag)
			{
			case RESOURCE_SHADER_MODULE:
			{
				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.shader_module != VK_NULL_HANDLE)
						vkDestroyShaderModule(device.get_device(), *work_item.hash_map_entry.shader_module, nullptr);
					*work_item.hash_map_entry.shader_module = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();
					if (vkCreateShaderModule(device.get_device(), work_item.create_info.shader_module_create_info,
					                         nullptr, work_item.output.shader_module) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
						shader_module_ns += duration_ns;
						shader_module_count++;
						*work_item.hash_map_entry.shader_module = *work_item.output.shader_module;
					}
					else
					{
						LOGE("Failed to create shader module for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}
				break;
			}

			case RESOURCE_GRAPHICS_PIPELINE:
			{
				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
						vkDestroyPipeline(device.get_device(), *work_item.hash_map_entry.pipeline, nullptr);
					*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();
					if (vkCreateGraphicsPipelines(device.get_device(), pipeline_cache, 1, work_item.create_info.graphics_create_info,
					                              nullptr, work_item.output.pipeline) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
						graphics_pipeline_ns += duration_ns;
						graphics_pipeline_count++;
						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;
					}
					else
					{
						LOGE("Failed to create graphics pipeline for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}
				break;
			}

			case RESOURCE_COMPUTE_PIPELINE:
			{
				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
						vkDestroyPipeline(device.get_device(), *work_item.hash_map_entry.pipeline, nullptr);
					*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();
					if (vkCreateComputePipelines(device.get_device(), pipeline_cache, 1,
					                             work_item.create_info.compute_create_info,
					                             nullptr, work_item.output.pipeline) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
						compute_pipeline_ns += duration_ns;
						compute_pipeline_count++;
						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;
					}
					else
					{
						LOGE("Failed to create compute pipeline for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}
				break;
			}

			default:
				break;
			}

			{
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				completed_count++;
				if (completed_count == queued_count) // Makes sense to signal main thread now.
					work_done_condition.notify_one();
			}
		}

		graphics_pipeline_count.fetch_add(graphics_count, std::memory_order_relaxed);
		graphics_pipeline_ns.fetch_add(graphics_ns, std::memory_order_relaxed);
		compute_pipeline_count.fetch_add(compute_count, std::memory_order_relaxed);
		compute_pipeline_ns.fetch_add(compute_ns, std::memory_order_relaxed);
		shader_module_count.fetch_add(shader_count, std::memory_order_relaxed);
		shader_module_ns.fetch_add(shader_ns, std::memory_order_relaxed);
	}

	~ThreadedReplayer()
	{
		// Signal that it's time for threads to die.
		{
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			shutting_down = true;
			work_available_condition.notify_all();
		}

		for (auto &thread : thread_pool)
			if (thread.joinable())
				thread.join();

		if (pipeline_cache)
		{
			if (!opts.on_disk_pipeline_cache_path.empty())
			{
				size_t pipeline_cache_size = 0;
				if (vkGetPipelineCacheData(device.get_device(), pipeline_cache, &pipeline_cache_size, nullptr) == VK_SUCCESS)
				{
					vector<uint8_t> pipeline_buffer(pipeline_cache_size);
					if (vkGetPipelineCacheData(device.get_device(), pipeline_cache, &pipeline_cache_size, pipeline_buffer.data()) == VK_SUCCESS)
					{
						FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "wb");
						if (!file || fwrite(pipeline_buffer.data(), 1, pipeline_buffer.size(), file) != pipeline_buffer.size())
							LOGE("Failed to write pipeline cache data to disk.\n");
						if (file)
							fclose(file);
					}
				}
			}
			vkDestroyPipelineCache(device.get_device(), pipeline_cache, nullptr);
		}

		for (auto &sampler : samplers)
			if (sampler.second)
				vkDestroySampler(device.get_device(), sampler.second, nullptr);
		for (auto &layout : layouts)
			if (layout.second)
				vkDestroyDescriptorSetLayout(device.get_device(), layout.second, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout.second)
				vkDestroyPipelineLayout(device.get_device(), pipeline_layout.second, nullptr);
		for (auto &shader_module : shader_modules)
			if (shader_module.second)
				vkDestroyShaderModule(device.get_device(), shader_module.second, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass.second)
				vkDestroyRenderPass(device.get_device(), render_pass.second, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device.get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device.get_device(), pipeline.second, nullptr);
	}

	bool validate_pipeline_cache_header(const vector<uint8_t> &blob)
	{
		if (blob.size() < 16 + VK_UUID_SIZE)
		{
			LOGI("Pipeline cache header is too small.\n");
			return false;
		}

		const auto read_le = [&](unsigned offset) -> uint32_t {
			return uint32_t(blob[offset + 0]) |
				(uint32_t(blob[offset + 1]) << 8) |
				(uint32_t(blob[offset + 2]) << 16) |
				(uint32_t(blob[offset + 3]) << 24);
		};

		auto length = read_le(0);
		if (length != 16 + VK_UUID_SIZE)
		{
			LOGI("Length of pipeline cache header is not as expected.\n");
			return false;
		}

		auto version = read_le(4);
		if (version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
		{
			LOGI("Version of pipeline cache header is not 1.\n");
			return false;
		}

		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(device.get_gpu(), &props);
		if (props.vendorID != read_le(8))
		{
			LOGI("Mismatch of vendorID and cache vendorID.\n");
			return false;
		}

		if (props.deviceID != read_le(12))
		{
			LOGI("Mismatch of deviceID and cache deviceID.\n");
			return false;
		}

		if (memcmp(props.pipelineCacheUUID, blob.data() + 16, VK_UUID_SIZE) != 0)
		{
			LOGI("Mismatch between pipelineCacheUUID.\n");
			return false;
		}

		return true;
	}

	void set_application_info(const VkApplicationInfo *app, const VkPhysicalDeviceFeatures2 *features) override
	{
		// TODO: Could use this to create multiple VkDevices for replay as necessary if app changes.

		if (!device_was_init)
		{
			// Now we can init the device with correct app info.
			device_was_init = true;
			device_opts.application_info = app;
			device_opts.features = features;
			device_opts.need_disasm = false;
			auto start_device = chrono::steady_clock::now();
			if (!device.init_device(device_opts))
			{
				LOGE("Failed to create Vulkan device, bailing ...\n");
				exit(EXIT_FAILURE);
			}

			if (opts.pipeline_cache)
			{
				VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
				vector<uint8_t> on_disk_cache;

				// Try to load on-disk cache.
				if (!opts.on_disk_pipeline_cache_path.empty())
				{
					FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "rb");
					if (file)
					{
						fseek(file, 0, SEEK_END);
						size_t len = ftell(file);
						rewind(file);

						if (len != 0)
						{
							on_disk_cache.resize(len);
							if (fread(on_disk_cache.data(), 1, len, file) == len)
							{
								if (validate_pipeline_cache_header(on_disk_cache))
								{
									info.pInitialData = on_disk_cache.data();
									info.initialDataSize = on_disk_cache.size();
								}
								else
									LOGI("Failed to validate pipeline cache. Creating a blank one.\n");
							}
						}
					}
				}

				if (vkCreatePipelineCache(device.get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
				{
					LOGE("Failed to create pipeline cache, trying to create a blank one.\n");
					info.initialDataSize = 0;
					info.pInitialData = nullptr;
					if (vkCreatePipelineCache(device.get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
					{
						LOGE("Failed to create pipeline cache.\n");
						pipeline_cache = VK_NULL_HANDLE;
					}
				}
			}

			auto end_device = chrono::steady_clock::now();
			long time_ms = chrono::duration_cast<chrono::milliseconds>(end_device - start_device).count();
			LOGI("Creating Vulkan device took: %ld ms\n", time_ms);

			if (app)
			{
				LOGI("Replaying for application:\n");
				LOGI("  apiVersion: %u.%u.%u\n",
				     VK_VERSION_MAJOR(app->apiVersion),
				     VK_VERSION_MINOR(app->apiVersion),
				     VK_VERSION_PATCH(app->apiVersion));
				LOGI("  engineVersion: %u\n", app->engineVersion);
				LOGI("  applicationVersion: %u\n", app->applicationVersion);
				if (app->pEngineName)
					LOGI("  engineName: %s\n", app->pEngineName);
				if (app->pApplicationName)
					LOGI("  applicationName: %s\n", app->pApplicationName);
			}
		}
	}

	bool enqueue_create_sampler(Hash index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		// Playback in-order.
		if (vkCreateSampler(device.get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %0" PRIX64 " Failed!\n", index);
			return false;
		}
		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		// Playback in-order.
		if (vkCreateDescriptorSetLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating descriptor set layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		// Playback in-order.
		if (vkCreatePipelineLayout(device.get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating pipeline layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		pipeline_layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_render_pass(Hash index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		// Playback in-order.
		if (vkCreateRenderPass(device.get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
		{
			LOGE("Creating render pass %0" PRIX64 " Failed!\n", index);
			return false;
		}
		render_passes[index] = *render_pass;
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		PipelineWorkItem work_item;
		work_item.hash = hash;
		work_item.tag = RESOURCE_SHADER_MODULE;
		work_item.output.shader_module = module;
		// Pointer to value in std::unordered_map remains fixed per spec (node-based).
		work_item.hash_map_entry.shader_module = &shader_modules[hash];
		work_item.create_info.shader_module_create_info = create_info;

		{
			// Pipeline parsing with pipeline creation.
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			pipeline_work_queue.push(work_item);
			work_available_condition.notify_one();
			queued_count++;
		}

		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if ((filter_compute.empty() && filter_graphics.empty()) || filter_compute.count(hash))
		{
			PipelineWorkItem work_item;
			work_item.hash = hash;
			work_item.tag = RESOURCE_COMPUTE_PIPELINE;
			work_item.output.pipeline = pipeline;
			// Pointer to value in std::unordered_map remains fixed per spec (node-based).
			work_item.hash_map_entry.pipeline = &compute_pipelines[hash];
			work_item.create_info.compute_create_info = create_info;

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}
		else
			*pipeline = VK_NULL_HANDLE;

		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		if ((filter_graphics.empty() && filter_compute.empty()) || filter_graphics.count(hash))
		{
			PipelineWorkItem work_item;
			work_item.hash = hash;
			work_item.tag = RESOURCE_GRAPHICS_PIPELINE;
			work_item.output.pipeline = pipeline;
			// Pointer to value in std::unordered_map remains fixed per spec (node-based).
			work_item.hash_map_entry.pipeline = &graphics_pipelines[hash];
			work_item.create_info.graphics_create_info = create_info;

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}
		else
			*pipeline = VK_NULL_HANDLE;

		return true;
	}

	void sync_threads() override
	{
		sync_worker_threads();
	}

	Options opts;
	const unordered_set<Hash> &filter_graphics;
	const unordered_set<Hash> &filter_compute;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> shader_modules;
	std::unordered_map<Hash, VkRenderPass> render_passes;
	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

	// VALVE: multi-threaded work queue for replayer
	struct PipelineWorkItem
	{
		Hash hash = 0;
		ResourceTag tag = RESOURCE_COUNT;

		union
		{
			const VkGraphicsPipelineCreateInfo *graphics_create_info;
			const VkComputePipelineCreateInfo *compute_create_info;
			const VkShaderModuleCreateInfo *shader_module_create_info;
		} create_info = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} output = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} hash_map_entry = {};
	};

	unsigned num_worker_threads = 0;
	unsigned loop_count = 0;
	unsigned queued_count = 0;
	unsigned completed_count = 0;
	std::vector<std::thread> thread_pool;
	std::mutex pipeline_work_queue_mutex;
	std::queue<PipelineWorkItem> pipeline_work_queue;
	std::condition_variable work_available_condition;
	std::condition_variable work_done_condition;

	// Feed statistics from the worker threads.
	std::atomic_uint64_t graphics_pipeline_ns;
	std::atomic_uint64_t compute_pipeline_ns;
	std::atomic_uint64_t shader_module_ns;
	std::atomic_uint graphics_pipeline_count;
	std::atomic_uint compute_pipeline_count;
	std::atomic_uint shader_module_count;

	bool shutting_down = false;

	VulkanDevice device;
	bool device_was_init = false;
	VulkanDevice::Options device_opts;
};

static void print_help()
{
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--pipeline-cache]\n"
	     "\t[--filter-compute <index>]\n"
	     "\t[--filter-graphics <index>]\n"
	     "\t[--num-threads <count>]\n"
	     "\t[--loop <count>]\n"
	     "\t[--on-disk-pipeline-cache <path>]\n"
	     "\t<Database>\n");
}

int main(int argc, char *argv[])
{
	string json_path;
	VulkanDevice::Options opts;
	ThreadedReplayer::Options replayer_opts;

	// TODO: Make this useable again.
	unordered_set<Hash> filter_graphics;
	unordered_set<Hash> filter_compute;

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { json_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
	cbs.add("--on-disk-pipeline-cache", [&](CLIParser &parser) { replayer_opts.on_disk_pipeline_cache_path = parser.next_string(); });
	cbs.add("--filter-compute", [&](CLIParser &parser) { filter_compute.insert(parser.next_uint()); });
	cbs.add("--filter-graphics", [&](CLIParser &parser) { filter_graphics.insert(parser.next_uint()); });
	cbs.add("--num-threads", [&](CLIParser &parser) { replayer_opts.num_threads = parser.next_uint(); });
	cbs.add("--loop", [&](CLIParser &parser) { replayer_opts.loop_count = parser.next_uint(); });
	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (json_path.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

	if (replayer_opts.num_threads < 1)
		replayer_opts.num_threads = 1;

	if (!replayer_opts.on_disk_pipeline_cache_path.empty())
		replayer_opts.pipeline_cache = true;

	auto start_time = chrono::steady_clock::now();
	ThreadedReplayer replayer(opts, replayer_opts, filter_graphics, filter_compute);

	auto start_create_archive = chrono::steady_clock::now();
	auto resolver = create_database(json_path, DatabaseMode::ReadOnly);
	auto end_create_archive = chrono::steady_clock::now();

	auto start_prepare = chrono::steady_clock::now();
	if (!resolver->prepare())
	{
		LOGE("Failed to prepare database.\n");
		return EXIT_FAILURE;
	}
	auto end_prepare = chrono::steady_clock::now();

	StateReplayer state_replayer;

	vector<Hash> resource_hashes;
	vector<uint8_t> state_json;

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO, // This will create the device, etc.
		RESOURCE_SHADER_MODULE, // Kick off shader modules first since it can be done in a thread while we deal with trivial objects.
		RESOURCE_SAMPLER, // Trivial, run in main thread.
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Trivial, run in main thread
		RESOURCE_PIPELINE_LAYOUT, // Trivial, run in main thread
		RESOURCE_RENDER_PASS, // Trivial, run in main thread
		RESOURCE_GRAPHICS_PIPELINE, // Multi-threaded
		RESOURCE_COMPUTE_PIPELINE, // Multi-threaded
	};

	for (auto &tag : playback_order)
	{
		if (!resolver->get_hash_list_for_resource_tag(tag, resource_hashes))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto &hash : resource_hashes)
		{
			if (!resolver->read_entry(tag, hash, state_json))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			try
			{
				state_replayer.parse(replayer, resolver.get(), state_json.data(), state_json.size());
			}
			catch (const exception &e)
			{
				LOGE("StateReplayer threw exception parsing (tag: %d, hash: 0x%llx): %s\n", tag, static_cast<unsigned long long>(hash), e.what());
			}
		}

		// Before continuing with pipelines, make sure the threaded shader modules have been created.
		if (tag == RESOURCE_RENDER_PASS)
			replayer.sync_worker_threads();
	}

	// VALVE: drain all outstanding pipeline compiles
	replayer.sync_worker_threads();

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.size() +
		replayer.render_passes.size() +
		replayer.compute_pipelines.size() +
		replayer.graphics_pipelines.size();

	long elapsed_ms_prepare = chrono::duration_cast<chrono::milliseconds>(end_prepare - start_prepare).count();
	long elapsed_ms_read_archive = chrono::duration_cast<chrono::milliseconds>(end_create_archive - start_create_archive).count();
	long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Opening archive took %ld ms:\n", elapsed_ms_read_archive);
	LOGI("Parsing archive took %ld ms:\n", elapsed_ms_prepare);

	LOGI("Playing back %u shader modules took %.3f s (accumulated time)\n",
	     replayer.shader_module_count.load(),
	     replayer.shader_module_ns.load() * 1e-9);

	LOGI("Playing back %u graphics pipelines took %.3f s (accumulated time)\n",
	     replayer.graphics_pipeline_count.load(),
	     replayer.graphics_pipeline_ns.load() * 1e-9);

	LOGI("Playing back %u compute pipelines took %.3f s (accumulated time)\n",
	     replayer.compute_pipeline_count.load(),
	     replayer.compute_pipeline_ns.load() * 1e-9);

	LOGI("Replayed %lu objects in %ld ms:\n", total_size, elapsed_ms);
	LOGI("  samplers:              %7lu\n", (unsigned long)replayer.samplers.size());
	LOGI("  descriptor set layouts:%7lu\n", (unsigned long)replayer.layouts.size());
	LOGI("  pipeline layouts:      %7lu\n", (unsigned long)replayer.pipeline_layouts.size());
	LOGI("  shader modules:        %7lu\n", (unsigned long)replayer.shader_modules.size());
	LOGI("  render passes:         %7lu\n", (unsigned long)replayer.render_passes.size());
	LOGI("  compute pipelines:     %7lu\n", (unsigned long)replayer.compute_pipelines.size());
	LOGI("  graphics pipelines:    %7lu\n", (unsigned long)replayer.graphics_pipelines.size());

	return EXIT_SUCCESS;
}
