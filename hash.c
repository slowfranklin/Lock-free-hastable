#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#define TRUE 1
#define FALSE 0

#define UNINITIALIZED (NULL)
typedef unsigned int key_t;

#define LOAD_FACTOR 0.75f

typedef struct _node node_t;

typedef node_t* mark_ptr_t;

struct _node {
	mark_ptr_t next;
	key_t key;
};

typedef struct {
	mark_ptr_t *table;
	unsigned count, size;
} conc_hashtable_t;

/*FIXME, make this use a table*/
static key_t
reverse_value (key_t k)
{
	int i;
	key_t r = 0;
	for (i = 0; i < 32; ++i) {
		key_t bit = (k & (1 << i)) >> i;
		r |= bit << (31 - i);
	}
	return r;
}

static inline key_t
hash_regular_key (key_t k)
{
	return reverse_value (k | 0x80000000);
}

static inline key_t
hash_dummy_key (key_t k)
{
	return reverse_value (k & ~0x80000000);
}

#define atomic_fetch_and_inc(t) __sync_fetch_and_add (t, 1)
#define atomic_fetch_and_dec(t) __sync_fetch_and_sub (t, 1)
#define atomic_compare_and_swap(t,old,new) __sync_bool_compare_and_swap (t, old, new)

#define load_barrier __builtin_ia32_lfence
#define store_barrier __builtin_ia32_sfence
#define memory_barrier __builtin_ia32_mfence

static inline mark_ptr_t
mk_node (node_t *n, uintptr_t bit)
{
	return  (mark_ptr_t)(((uintptr_t)n) | bit);
}

static inline node_t*
get_node (mark_ptr_t n)
{
	return  (node_t*)(((uintptr_t)n) & ~(uintptr_t)0x1);
}

static inline uintptr_t
get_bit (mark_ptr_t n)
{
	return  (uintptr_t)n & 0x1;
}

static void
delete_node (mark_ptr_t node)
{
	assert (get_bit (node) == 0);
	free (get_node (node));
}

static mark_ptr_t cur, next, *prev;

#define atomic_load(v, p)  do { load_barrier (); v = *(p); } while (0)
#define atomic_store(p, h) do { store_barrier (); *(p) = v; } while (0);

int
list_find (mark_ptr_t *head, key_t key)
{
try_again:
	prev = head;
	atomic_load (cur, prev);
	while (1) {
		mark_ptr_t tmp;
		if (cur == NULL)
			return FALSE;
		next = cur->next;
		key_t cur_key = cur->key;
		atomic_load (tmp, prev);

		if (tmp != mk_node (get_node (cur), 0))
			goto try_again;

		if (!get_bit (next)) {
			if (cur_key >= key)
				return cur_key == key;
			prev = &get_node (cur)->next;
		} else {
			if (atomic_compare_and_swap (prev, mk_node (get_node (cur), 0), mk_node (get_node (next), 0)))
				delete_node (get_node (cur));
			else
				goto try_again;
		}
		cur = next;
	}
}

int
list_insert (mark_ptr_t *head, node_t *node)
{
	key_t key = node->key;

	while (1) {
		if (list_find (head, key))
			return FALSE;
		node->next = mk_node (get_node (cur), 0);
		if (atomic_compare_and_swap (prev, mk_node (get_node (cur), 0), mk_node (node, 0)))
			return TRUE;
	}
}

int
list_delete (mark_ptr_t *head, key_t key)
{
	while (1) {
		if (!list_find (head, key))
			return FALSE;
		if (!atomic_compare_and_swap (&get_node (cur)->next, mk_node (get_node (next), 0), mk_node (get_node (next), 1)))
			continue;
		if (atomic_compare_and_swap (prev, mk_node (get_node (cur), 0), mk_node (get_node (next), 0)))
			delete_node (get_node (cur));
		else
			list_find (head, key);
		return TRUE;
	}
}

static unsigned
get_parent (unsigned b)
{
	int i;
	for (i = 31; i >= 0; --i) {
		if (b & (1 << i))
			return b & ~(1 << i);
	}
	return 0;
}

static void
initialize_bucket (conc_hashtable_t *ht, unsigned bucket)
{
	unsigned parent = get_parent (bucket);
	if (ht->table [parent] == UNINITIALIZED)
		initialize_bucket (ht, parent);

	node_t *node = calloc (sizeof (node), 1);
	node->key = hash_dummy_key (bucket);

	if (!list_insert (&ht->table [parent], node)) {
		free (node);
		node = get_node (cur);
	}

	store_barrier ();
	ht->table [bucket] = mk_node (node, 0);
}

static void
resize_table (conc_hashtable_t *ht, unsigned size)
{
	node_t **old_table = ht->table;
	node_t **new_table = calloc (sizeof (node_t*), size * 2);
	memcpy (new_table, old_table, sizeof (node_t*) * size);
	if (!atomic_compare_and_swap (&ht->size, size, size * 2)) {
		free (new_table);
		return;
	}
	if (!atomic_compare_and_swap ((void**)&ht->table, old_table, new_table))
		free (new_table);
}

static int /*BOOL*/
insert (conc_hashtable_t *ht, key_t key)
{
	node_t *node = calloc (sizeof (node), 1);
	node->key = hash_regular_key (key);

	unsigned bucket = key % ht->size;

	if (ht->table [bucket] == UNINITIALIZED)
		initialize_bucket (ht, bucket);
	if (!list_insert (&ht->table [bucket], node)) {
		free (node);
		return FALSE;
	}

	float size = (float)ht->size;
	if (atomic_fetch_and_inc (&ht->count) / size > LOAD_FACTOR)
		resize_table (ht, size);

	return TRUE;
}

static int
find (conc_hashtable_t *ht, key_t key)
{
	unsigned bucket = key % ht->size;
	if (ht->table [bucket] == UNINITIALIZED)
		initialize_bucket (ht, bucket);
	return list_find (&ht->table [bucket], hash_regular_key (key));
}

static int
delete (conc_hashtable_t *ht, key_t key)
{
	unsigned bucket = key % ht->size;
	if (ht->table [bucket] == UNINITIALIZED)
		initialize_bucket (ht, bucket);

	if (!list_delete (&ht->table [bucket], hash_regular_key (key)))
		return FALSE;

	atomic_fetch_and_dec (&ht->count);
	return TRUE;
}

conc_hashtable_t*
create (void)
{
	conc_hashtable_t *res = calloc (sizeof (conc_hashtable_t), 1);
	res->size = 16;
	res->table = calloc (sizeof (node_t), 16);
	res->table [0] = calloc (sizeof (node_t), 1);
	res->table [0]->key = hash_dummy_key (0);
	return res;
}

int main ()
{
	int i = 0;
	conc_hashtable_t *ht = create ();

	printf ("find %d %d\n", find (ht, 0), find (ht, 10));
	insert (ht, 0);
	insert (ht, 26);
	printf ("find %d %d\n", find (ht, 0), find (ht, 10));
	delete (ht, 0);
	printf ("find %d %d\n", find (ht, 0), find (ht, 10));
	
	printf ("%d ", insert (ht, 5));
	printf ("%d ", insert (ht, 5));
	printf ("%d ", insert (ht, 5));
	printf ("%d\n", ht->count);

	for (i = 0; i < 50; ++i)
		insert (ht, i);
	for (i = 0; i < 50; ++i)
		printf ("[%d] = %d\n", i, find (ht, i));
	printf ("total %d\n", ht->count);
	return 0;
}