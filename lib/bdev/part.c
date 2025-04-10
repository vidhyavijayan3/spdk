/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   Copyright (c) 2022 NVIDIA CORPORATION & AFFILIATES.
 *   All rights reserved.
 */

/*
 * Common code for partition-like virtual bdevs.
 */

#include "spdk/bdev.h"
#include "spdk/likely.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/thread.h"

#include "spdk/bdev_module.h"

/* This namespace UUID was generated using uuid_generate() method. */
#define BDEV_PART_NAMESPACE_UUID "976b899e-3e1e-4d71-ab69-c2b08e9df8b8"

struct spdk_bdev_part_base {
	struct spdk_bdev		*bdev;
	struct spdk_bdev_desc		*desc;
	uint32_t			ref;
	uint32_t			channel_size;
	spdk_bdev_part_base_free_fn	base_free_fn;
	void				*ctx;
	bool				claimed;
	struct spdk_bdev_module		*module;
	struct spdk_bdev_fn_table	*fn_table;
	struct bdev_part_tailq		*tailq;
	spdk_io_channel_create_cb	ch_create_cb;
	spdk_io_channel_destroy_cb	ch_destroy_cb;
	spdk_bdev_remove_cb_t		remove_cb;
	struct spdk_thread		*thread;
};

struct spdk_bdev *
spdk_bdev_part_base_get_bdev(struct spdk_bdev_part_base *part_base)
{
	return part_base->bdev;
}

struct spdk_bdev_desc *
spdk_bdev_part_base_get_desc(struct spdk_bdev_part_base *part_base)
{
	return part_base->desc;
}

struct bdev_part_tailq *
spdk_bdev_part_base_get_tailq(struct spdk_bdev_part_base *part_base)
{
	return part_base->tailq;
}

void *
spdk_bdev_part_base_get_ctx(struct spdk_bdev_part_base *part_base)
{
	return part_base->ctx;
}

const char *
spdk_bdev_part_base_get_bdev_name(struct spdk_bdev_part_base *part_base)
{
	return part_base->bdev->name;
}

static void
bdev_part_base_free(void *ctx)
{
	struct spdk_bdev_desc *desc = ctx;

	spdk_bdev_close(desc);
}

void
spdk_bdev_part_base_free(struct spdk_bdev_part_base *base)
{
	if (base->desc) {
		/* Close the underlying bdev on its same opened thread. */
		if (base->thread && base->thread != spdk_get_thread()) {
			spdk_thread_send_msg(base->thread, bdev_part_base_free, base->desc);
		} else {
			spdk_bdev_close(base->desc);
		}
	}

	if (base->base_free_fn != NULL) {
		base->base_free_fn(base->ctx);
	}

	free(base);
}

static void
bdev_part_free_cb(void *io_device)
{
	struct spdk_bdev_part *part = io_device;
	struct spdk_bdev_part_base *base;

	assert(part);
	assert(part->internal.base);

	base = part->internal.base;

	TAILQ_REMOVE(base->tailq, part, tailq);

	if (--base->ref == 0) {
		spdk_bdev_module_release_bdev(base->bdev);
		spdk_bdev_part_base_free(base);
	}

	spdk_bdev_destruct_done(&part->internal.bdev, 0);
	free(part->internal.bdev.name);
	free(part->internal.bdev.product_name);
	free(part);
}

int
spdk_bdev_part_free(struct spdk_bdev_part *part)
{
	spdk_io_device_unregister(part, bdev_part_free_cb);

	/* Return 1 to indicate that this is an asynchronous operation that isn't complete
	 * until spdk_bdev_destruct_done is called */
	return 1;
}

void
spdk_bdev_part_base_hotremove(struct spdk_bdev_part_base *part_base, struct bdev_part_tailq *tailq)
{
	struct spdk_bdev_part *part, *tmp;

	TAILQ_FOREACH_SAFE(part, tailq, tailq, tmp) {
		if (part->internal.base == part_base) {
			spdk_bdev_unregister(&part->internal.bdev, NULL, NULL);
		}
	}
}

static bool
bdev_part_io_type_supported(void *_part, enum spdk_bdev_io_type io_type)
{
	struct spdk_bdev_part *part = _part;

	/* We can't decode/modify passthrough NVMe commands, so don't report
	 *  that a partition supports these io types, even if the underlying
	 *  bdev does.
	 */
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_NVME_ADMIN:
	case SPDK_BDEV_IO_TYPE_NVME_IO:
	case SPDK_BDEV_IO_TYPE_NVME_IO_MD:
		return false;
	default:
		break;
	}

	return part->internal.base->bdev->fn_table->io_type_supported(part->internal.base->bdev->ctxt,
			io_type);
}

static struct spdk_io_channel *
bdev_part_get_io_channel(void *_part)
{
	struct spdk_bdev_part *part = _part;

	return spdk_get_io_channel(part);
}

struct spdk_bdev *
spdk_bdev_part_get_bdev(struct spdk_bdev_part *part)
{
	return &part->internal.bdev;
}

struct spdk_bdev_part_base *
spdk_bdev_part_get_base(struct spdk_bdev_part *part)
{
	return part->internal.base;
}

struct spdk_bdev *
spdk_bdev_part_get_base_bdev(struct spdk_bdev_part *part)
{
	return part->internal.base->bdev;
}

uint64_t
spdk_bdev_part_get_offset_blocks(struct spdk_bdev_part *part)
{
	return part->internal.offset_blocks;
}

static int
bdev_part_remap_dif(struct spdk_bdev_io *bdev_io, uint32_t offset,
		    uint32_t remapped_offset)
{
	struct spdk_bdev *bdev = bdev_io->bdev;
	struct spdk_dif_ctx dif_ctx;
	struct spdk_dif_error err_blk = {};
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	if (spdk_likely(!(bdev_io->u.bdev.dif_check_flags & SPDK_DIF_FLAGS_REFTAG_CHECK))) {
		return 0;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	rc = spdk_dif_ctx_init(&dif_ctx,
			       bdev->blocklen, bdev->md_len, bdev->md_interleave,
			       bdev->dif_is_head_of_md, bdev->dif_type, bdev_io->u.bdev.dif_check_flags,
			       offset, 0, 0, 0, 0, &dif_opts);
	if (rc != 0) {
		SPDK_ERRLOG("Initialization of DIF context failed\n");
		return rc;
	}

	spdk_dif_ctx_set_remapped_init_ref_tag(&dif_ctx, remapped_offset);

	if (bdev->md_interleave) {
		rc = spdk_dif_remap_ref_tag(bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
					    bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk, true);
	} else {
		struct iovec md_iov = {
			.iov_base	= bdev_io->u.bdev.md_buf,
			.iov_len	= bdev_io->u.bdev.num_blocks * bdev->md_len,
		};

		rc = spdk_dix_remap_ref_tag(&md_iov, bdev_io->u.bdev.num_blocks, &dif_ctx, &err_blk, true);
	}

	if (rc != 0) {
		SPDK_ERRLOG("Remapping reference tag failed. type=%d, offset=%" PRIu32 "\n",
			    err_blk.err_type, err_blk.err_offset);
	}

	return rc;
}

static void
bdev_part_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *part_io = cb_arg;
	uint32_t offset, remapped_offset;
	int rc;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (success) {
			offset = bdev_io->u.bdev.offset_blocks;
			remapped_offset = part_io->u.bdev.offset_blocks;

			rc = bdev_part_remap_dif(bdev_io, offset, remapped_offset);
			if (rc != 0) {
				success = false;
			}
		}
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		spdk_bdev_io_set_buf(part_io, bdev_io->u.bdev.iovs[0].iov_base,
				     bdev_io->u.bdev.iovs[0].iov_len);
		break;
	default:
		break;
	}

	if (part_io->internal.f.split) {
		part_io->internal.split.stored_user_cb(part_io, success, NULL);
	} else {
		spdk_bdev_io_complete_base_io_status(part_io, bdev_io);
	}

	spdk_bdev_free_io(bdev_io);
}

static inline void
bdev_part_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	opts->metadata = bdev_io->u.bdev.md_buf;
	opts->dif_check_flags_exclude_mask = ~bdev_io->u.bdev.dif_check_flags;
}

int
spdk_bdev_part_submit_request_ext(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io,
				  spdk_bdev_io_completion_cb cb)
{
	struct spdk_bdev_part *part = ch->part;
	struct spdk_io_channel *base_ch = ch->base_ch;
	struct spdk_bdev_desc *base_desc = part->internal.base->desc;
	struct spdk_bdev_ext_io_opts io_opts;
	uint64_t offset, remapped_offset, remapped_src_offset;
	int rc = 0;

	if (cb != NULL) {
		bdev_io->internal.f.split = true;
		bdev_io->internal.split.stored_user_cb = cb;
	}

	offset = bdev_io->u.bdev.offset_blocks;
	remapped_offset = offset + part->internal.offset_blocks;

	/* Modify the I/O to adjust for the offset within the base bdev. */
	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		bdev_part_init_ext_io_opts(bdev_io, &io_opts);
		rc = spdk_bdev_readv_blocks_ext(base_desc, base_ch, bdev_io->u.bdev.iovs,
						bdev_io->u.bdev.iovcnt, remapped_offset,
						bdev_io->u.bdev.num_blocks,
						bdev_part_complete_io, bdev_io, &io_opts);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = bdev_part_remap_dif(bdev_io, offset, remapped_offset);
		if (rc != 0) {
			return SPDK_BDEV_IO_STATUS_FAILED;
		}
		bdev_part_init_ext_io_opts(bdev_io, &io_opts);
		rc = spdk_bdev_writev_blocks_ext(base_desc, base_ch, bdev_io->u.bdev.iovs,
						 bdev_io->u.bdev.iovcnt, remapped_offset,
						 bdev_io->u.bdev.num_blocks,
						 bdev_part_complete_io, bdev_io, &io_opts);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(base_desc, base_ch, remapped_offset,
						   bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
						   bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(base_desc, base_ch, remapped_offset,
					    bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(base_desc, base_ch, remapped_offset,
					    bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					    bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(base_desc, base_ch,
				     bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = spdk_bdev_abort(base_desc, base_ch, bdev_io->u.abort.bio_to_abort,
				     bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_ZCOPY:
		rc = spdk_bdev_zcopy_start(base_desc, base_ch, NULL, 0, remapped_offset,
					   bdev_io->u.bdev.num_blocks, bdev_io->u.bdev.zcopy.populate,
					   bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE:
		if (!bdev_io->u.bdev.md_buf) {
			rc = spdk_bdev_comparev_blocks(base_desc, base_ch,
						       bdev_io->u.bdev.iovs,
						       bdev_io->u.bdev.iovcnt,
						       remapped_offset,
						       bdev_io->u.bdev.num_blocks,
						       bdev_part_complete_io, bdev_io);
		} else {
			rc = spdk_bdev_comparev_blocks_with_md(base_desc, base_ch,
							       bdev_io->u.bdev.iovs,
							       bdev_io->u.bdev.iovcnt,
							       bdev_io->u.bdev.md_buf,
							       remapped_offset,
							       bdev_io->u.bdev.num_blocks,
							       bdev_part_complete_io, bdev_io);
		}
		break;
	case SPDK_BDEV_IO_TYPE_COMPARE_AND_WRITE:
		rc = spdk_bdev_comparev_and_writev_blocks(base_desc, base_ch, bdev_io->u.bdev.iovs,
				bdev_io->u.bdev.iovcnt,
				bdev_io->u.bdev.fused_iovs,
				bdev_io->u.bdev.fused_iovcnt,
				remapped_offset,
				bdev_io->u.bdev.num_blocks,
				bdev_part_complete_io, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		remapped_src_offset = bdev_io->u.bdev.copy.src_offset_blocks + part->internal.offset_blocks;
		rc = spdk_bdev_copy_blocks(base_desc, base_ch, remapped_offset, remapped_src_offset,
					   bdev_io->u.bdev.num_blocks, bdev_part_complete_io,
					   bdev_io);
		break;
	default:
		SPDK_ERRLOG("unknown I/O type %d\n", bdev_io->type);
		return SPDK_BDEV_IO_STATUS_FAILED;
	}

	return rc;
}

int
spdk_bdev_part_submit_request(struct spdk_bdev_part_channel *ch, struct spdk_bdev_io *bdev_io)
{
	return spdk_bdev_part_submit_request_ext(ch, bdev_io, NULL);
}

static int
bdev_part_channel_create_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = (struct spdk_bdev_part *)io_device;
	struct spdk_bdev_part_channel *ch = ctx_buf;

	ch->part = part;
	ch->base_ch = spdk_bdev_get_io_channel(part->internal.base->desc);
	if (ch->base_ch == NULL) {
		return -1;
	}

	if (part->internal.base->ch_create_cb) {
		return part->internal.base->ch_create_cb(io_device, ctx_buf);
	} else {
		return 0;
	}
}

static void
bdev_part_channel_destroy_cb(void *io_device, void *ctx_buf)
{
	struct spdk_bdev_part *part = (struct spdk_bdev_part *)io_device;
	struct spdk_bdev_part_channel *ch = ctx_buf;

	if (part->internal.base->ch_destroy_cb) {
		part->internal.base->ch_destroy_cb(io_device, ctx_buf);
	}
	spdk_put_io_channel(ch->base_ch);
}

static void
bdev_part_base_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			void *event_ctx)
{
	struct spdk_bdev_part_base *base = event_ctx;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		base->remove_cb(base);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

int
spdk_bdev_part_base_construct_ext(const char *bdev_name,
				  spdk_bdev_remove_cb_t remove_cb, struct spdk_bdev_module *module,
				  struct spdk_bdev_fn_table *fn_table, struct bdev_part_tailq *tailq,
				  spdk_bdev_part_base_free_fn free_fn, void *ctx,
				  uint32_t channel_size, spdk_io_channel_create_cb ch_create_cb,
				  spdk_io_channel_destroy_cb ch_destroy_cb,
				  struct spdk_bdev_part_base **_base)
{
	int rc;
	struct spdk_bdev_part_base *base;

	if (_base == NULL) {
		return -EINVAL;
	}

	base = calloc(1, sizeof(*base));
	if (!base) {
		SPDK_ERRLOG("Memory allocation failure\n");
		return -ENOMEM;
	}
	fn_table->get_io_channel = bdev_part_get_io_channel;
	fn_table->io_type_supported = bdev_part_io_type_supported;

	base->desc = NULL;
	base->ref = 0;
	base->module = module;
	base->fn_table = fn_table;
	base->tailq = tailq;
	base->base_free_fn = free_fn;
	base->ctx = ctx;
	base->claimed = false;
	base->channel_size = channel_size;
	base->ch_create_cb = ch_create_cb;
	base->ch_destroy_cb = ch_destroy_cb;
	base->remove_cb = remove_cb;

	rc = spdk_bdev_open_ext(bdev_name, false, bdev_part_base_event_cb, base, &base->desc);
	if (rc) {
		if (rc == -ENODEV) {
			free(base);
		} else {
			SPDK_ERRLOG("could not open bdev %s: %s\n", bdev_name, spdk_strerror(-rc));
			spdk_bdev_part_base_free(base);
		}
		return rc;
	}

	base->bdev = spdk_bdev_desc_get_bdev(base->desc);

	/* Save the thread where the base device is opened */
	base->thread = spdk_get_thread();

	*_base = base;

	return 0;
}

void
spdk_bdev_part_construct_opts_init(struct spdk_bdev_part_construct_opts *opts, uint64_t size)
{
	if (opts == NULL) {
		SPDK_ERRLOG("opts should not be NULL\n");
		assert(opts != NULL);
		return;
	}
	if (size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		assert(size != 0);
		return;
	}

	memset(opts, 0, size);
	opts->opts_size = size;
}

static void
part_construct_opts_copy(const struct spdk_bdev_part_construct_opts *src,
			 struct spdk_bdev_part_construct_opts *dst)
{
	if (src->opts_size == 0) {
		SPDK_ERRLOG("size should not be zero\n");
		assert(false);
	}

	memset(dst, 0, sizeof(*dst));
	dst->opts_size = src->opts_size;

#define FIELD_OK(field) \
        offsetof(struct spdk_bdev_part_construct_opts, field) + sizeof(src->field) <= src->opts_size

#define SET_FIELD(field) \
        if (FIELD_OK(field)) { \
                dst->field = src->field; \
        } \

	SET_FIELD(uuid);

	/* You should not remove this statement, but need to update the assert statement
	 * if you add a new field, and also add a corresponding SET_FIELD statement */
	SPDK_STATIC_ASSERT(sizeof(struct spdk_bdev_part_construct_opts) == 24, "Incorrect size");

#undef FIELD_OK
#undef SET_FIELD
}

int
spdk_bdev_part_construct_ext(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			     char *name, uint64_t offset_blocks, uint64_t num_blocks,
			     char *product_name, const struct spdk_bdev_part_construct_opts *_opts)
{
	int rc;
	bool first_claimed = false;
	struct spdk_bdev_part_construct_opts opts;
	struct spdk_uuid ns_uuid;

	if (_opts == NULL) {
		spdk_bdev_part_construct_opts_init(&opts, sizeof(opts));
	} else {
		part_construct_opts_copy(_opts, &opts);
	}

	part->internal.bdev.blocklen = base->bdev->blocklen;
	part->internal.bdev.blockcnt = num_blocks;
	part->internal.offset_blocks = offset_blocks;

	part->internal.bdev.write_cache = base->bdev->write_cache;
	part->internal.bdev.required_alignment = base->bdev->required_alignment;
	part->internal.bdev.ctxt = part;
	part->internal.bdev.module = base->module;
	part->internal.bdev.fn_table = base->fn_table;

	part->internal.bdev.md_interleave = base->bdev->md_interleave;
	part->internal.bdev.md_len = base->bdev->md_len;
	part->internal.bdev.dif_type = base->bdev->dif_type;
	part->internal.bdev.dif_is_head_of_md = base->bdev->dif_is_head_of_md;
	part->internal.bdev.dif_check_flags = base->bdev->dif_check_flags;

	part->internal.bdev.name = strdup(name);
	if (part->internal.bdev.name == NULL) {
		SPDK_ERRLOG("Failed to allocate name for new part of bdev %s\n", spdk_bdev_get_name(base->bdev));
		return -1;
	}

	part->internal.bdev.product_name = strdup(product_name);
	if (part->internal.bdev.product_name == NULL) {
		free(part->internal.bdev.name);
		SPDK_ERRLOG("Failed to allocate product name for new part of bdev %s\n",
			    spdk_bdev_get_name(base->bdev));
		return -1;
	}

	/* The caller may have already specified a UUID.  If not, we'll generate one
	 * based on the namespace UUID, the base bdev's UUID and the block range of the
	 * partition.
	 */
	if (!spdk_uuid_is_null(&opts.uuid)) {
		spdk_uuid_copy(&part->internal.bdev.uuid, &opts.uuid);
	} else {
		struct {
			struct spdk_uuid	uuid;
			uint64_t		offset_blocks;
			uint64_t		num_blocks;
		} base_name;

		/* We need to create a unique base name for this partition.  We can't just use
		 * the base bdev's UUID, since it may be used for multiple partitions.  So
		 * construct a binary name consisting of the uuid + the block range for this
		 * partition.
		 */
		spdk_uuid_copy(&base_name.uuid, &base->bdev->uuid);
		base_name.offset_blocks = offset_blocks;
		base_name.num_blocks = num_blocks;

		spdk_uuid_parse(&ns_uuid, BDEV_PART_NAMESPACE_UUID);
		rc = spdk_uuid_generate_sha1(&part->internal.bdev.uuid, &ns_uuid,
					     (const char *)&base_name, sizeof(base_name));
		if (rc) {
			SPDK_ERRLOG("Could not generate new UUID\n");
			free(part->internal.bdev.name);
			free(part->internal.bdev.product_name);
			return -1;
		}
	}

	base->ref++;
	part->internal.base = base;

	if (!base->claimed) {
		int rc;

		rc = spdk_bdev_module_claim_bdev(base->bdev, base->desc, base->module);
		if (rc) {
			SPDK_ERRLOG("could not claim bdev %s\n", spdk_bdev_get_name(base->bdev));
			free(part->internal.bdev.name);
			free(part->internal.bdev.product_name);
			base->ref--;
			return -1;
		}
		base->claimed = true;
		first_claimed = true;
	}

	spdk_io_device_register(part, bdev_part_channel_create_cb,
				bdev_part_channel_destroy_cb,
				base->channel_size,
				name);

	rc = spdk_bdev_register(&part->internal.bdev);
	if (rc == 0) {
		TAILQ_INSERT_TAIL(base->tailq, part, tailq);
	} else {
		spdk_io_device_unregister(part, NULL);
		if (--base->ref == 0) {
			spdk_bdev_module_release_bdev(base->bdev);
		}
		free(part->internal.bdev.name);
		free(part->internal.bdev.product_name);
		if (first_claimed == true) {
			base->claimed = false;
		}
	}

	return rc;
}

int
spdk_bdev_part_construct(struct spdk_bdev_part *part, struct spdk_bdev_part_base *base,
			 char *name, uint64_t offset_blocks, uint64_t num_blocks,
			 char *product_name)
{
	return spdk_bdev_part_construct_ext(part, base, name, offset_blocks, num_blocks,
					    product_name, NULL);
}
