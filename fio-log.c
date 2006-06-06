#include <stdio.h>
#include <stdlib.h>
#include "list.h"
#include "fio.h"

void write_iolog_put(struct thread_data *td, struct io_u *io_u)
{
	fprintf(td->iolog_f, "%d,%llu,%u\n", io_u->ddir, io_u->offset, io_u->buflen);
}

int read_iolog_get(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo;

	if (!list_empty(&td->io_log_list)) {
		ipo = list_entry(td->io_log_list.next, struct io_piece, list);
		list_del(&ipo->list);
		io_u->offset = ipo->offset;
		io_u->buflen = ipo->len;
		io_u->ddir = ipo->ddir;
		free(ipo);
		return 0;
	}

	return 1;
}

void prune_io_piece_log(struct thread_data *td)
{
	struct io_piece *ipo;

	while (!list_empty(&td->io_hist_list)) {
		ipo = list_entry(td->io_hist_list.next, struct io_piece, list);

		list_del(&ipo->list);
		free(ipo);
	}
}

/*
 * log a succesful write, so we can unwind the log for verify
 */
void log_io_piece(struct thread_data *td, struct io_u *io_u)
{
	struct io_piece *ipo = malloc(sizeof(struct io_piece));
	struct list_head *entry;

	INIT_LIST_HEAD(&ipo->list);
	ipo->offset = io_u->offset;
	ipo->len = io_u->buflen;

	/*
	 * for random io where the writes extend the file, it will typically
	 * be laid out with the block scattered as written. it's faster to
	 * read them in in that order again, so don't sort
	 */
	if (td->sequential || !td->overwrite) {
		list_add_tail(&ipo->list, &td->io_hist_list);
		return;
	}

	/*
	 * for random io, sort the list so verify will run faster
	 */
	entry = &td->io_hist_list;
	while ((entry = entry->prev) != &td->io_hist_list) {
		struct io_piece *__ipo = list_entry(entry, struct io_piece, list);

		if (__ipo->offset < ipo->offset)
			break;
	}

	list_add(&ipo->list, entry);
}

void write_iolog_close(struct thread_data *td)
{
	fflush(td->iolog_f);
	fclose(td->iolog_f);
	free(td->iolog_buf);
}

int init_iolog(struct thread_data *td)
{
	unsigned long long offset;
	unsigned int bytes;
	char *str, *p;
	FILE *f;
	int rw, i, reads, writes;

	if (!td->read_iolog && !td->write_iolog)
		return 0;

	if (td->read_iolog)
		f = fopen(td->iolog_file, "r");
	else
		f = fopen(td->iolog_file, "w");

	if (!f) {
		perror("fopen iolog");
		printf("file %s, %d/%d\n", td->iolog_file, td->read_iolog, td->write_iolog);
		return 1;
	}

	/*
	 * That's it for writing, setup a log buffer and we're done.
	  */
	if (td->write_iolog) {
		td->iolog_f = f;
		td->iolog_buf = malloc(8192);
		setvbuf(f, td->iolog_buf, _IOFBF, 8192);
		return 0;
	}

	/*
	 * Read in the read iolog and store it, reuse the infrastructure
	 * for doing verifications.
	 */
	str = malloc(4096);
	reads = writes = i = 0;
	while ((p = fgets(str, 4096, f)) != NULL) {
		struct io_piece *ipo;

		if (sscanf(p, "%d,%llu,%u", &rw, &offset, &bytes) != 3) {
			fprintf(stderr, "bad iolog: %s\n", p);
			continue;
		}
		if (rw == DDIR_READ)
			reads++;
		else if (rw == DDIR_WRITE)
			writes++;
		else {
			fprintf(stderr, "bad ddir: %d\n", rw);
			continue;
		}

		ipo = malloc(sizeof(*ipo));
		INIT_LIST_HEAD(&ipo->list);
		ipo->offset = offset;
		ipo->len = bytes;
		if (bytes > td->max_bs)
			td->max_bs = bytes;
		ipo->ddir = rw;
		list_add_tail(&ipo->list, &td->io_log_list);
		i++;
	}

	free(str);
	fclose(f);

	if (!i)
		return 1;

	if (reads && !writes)
		td->ddir = DDIR_READ;
	else if (!reads && writes)
		td->ddir = DDIR_READ;
	else
		td->iomix = 1;

	return 0;
}