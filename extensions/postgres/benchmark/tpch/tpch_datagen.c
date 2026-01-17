/*
 * Orochi DB - TPC-H Data Generator
 *
 * Generates TPC-H-compliant data for benchmarking.
 * Based on TPC-H Specification 3.0.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "bench_common.h"

/* TPC-H scale factors */
#define SF_1GB_LINEITEM     6001215
#define SF_1GB_ORDERS       1500000
#define SF_1GB_CUSTOMER     150000
#define SF_1GB_PART         200000
#define SF_1GB_PARTSUPP     800000
#define SF_1GB_SUPPLIER     10000
#define SF_1GB_NATION       25
#define SF_1GB_REGION       5

/* Data constants */
static const char *regions[] = {
    "AFRICA", "AMERICA", "ASIA", "EUROPE", "MIDDLE EAST"
};

static const char *nations[] = {
    "ALGERIA", "ARGENTINA", "BRAZIL", "CANADA", "EGYPT",
    "ETHIOPIA", "FRANCE", "GERMANY", "INDIA", "INDONESIA",
    "IRAN", "IRAQ", "JAPAN", "JORDAN", "KENYA",
    "MOROCCO", "MOZAMBIQUE", "PERU", "CHINA", "ROMANIA",
    "SAUDI ARABIA", "VIETNAM", "RUSSIA", "UNITED KINGDOM", "UNITED STATES"
};

static const int nation_regions[] = {
    0, 1, 1, 1, 4, 0, 3, 3, 2, 2,
    4, 4, 2, 4, 0, 0, 0, 1, 2, 3,
    4, 2, 3, 3, 1
};

static const char *segments[] = {
    "AUTOMOBILE", "BUILDING", "FURNITURE", "HOUSEHOLD", "MACHINERY"
};

static const char *priorities[] = {
    "1-URGENT", "2-HIGH", "3-MEDIUM", "4-NOT SPECIFIED", "5-LOW"
};

static const char *ship_modes[] = {
    "REG AIR", "AIR", "RAIL", "SHIP", "TRUCK", "MAIL", "FOB"
};

static const char *ship_instruct[] = {
    "DELIVER IN PERSON", "COLLECT COD", "NONE", "TAKE BACK RETURN"
};

static const char *containers[] = {
    "SM CASE", "SM BOX", "SM PACK", "SM PKG", "SM BAG", "SM JAR", "SM DRUM", "SM CAN",
    "MED CASE", "MED BOX", "MED PACK", "MED PKG", "MED BAG", "MED JAR", "MED DRUM", "MED CAN",
    "LG CASE", "LG BOX", "LG PACK", "LG PKG", "LG BAG", "LG JAR", "LG DRUM", "LG CAN",
    "JUMBO CASE", "JUMBO BOX", "JUMBO PACK", "JUMBO PKG", "JUMBO BAG", "JUMBO JAR", "JUMBO DRUM", "JUMBO CAN",
    "WRAP CASE", "WRAP BOX", "WRAP PACK", "WRAP PKG", "WRAP BAG", "WRAP JAR", "WRAP DRUM", "WRAP CAN"
};

static const char *types_syllable1[] = {
    "STANDARD", "SMALL", "MEDIUM", "LARGE", "ECONOMY", "PROMO"
};

static const char *types_syllable2[] = {
    "ANODIZED", "BURNISHED", "PLATED", "POLISHED", "BRUSHED"
};

static const char *types_syllable3[] = {
    "TIN", "NICKEL", "BRASS", "STEEL", "COPPER"
};

static const char *brands[] = {
    "Brand#11", "Brand#12", "Brand#13", "Brand#14", "Brand#15",
    "Brand#21", "Brand#22", "Brand#23", "Brand#24", "Brand#25",
    "Brand#31", "Brand#32", "Brand#33", "Brand#34", "Brand#35",
    "Brand#41", "Brand#42", "Brand#43", "Brand#44", "Brand#45",
    "Brand#51", "Brand#52", "Brand#53", "Brand#54", "Brand#55"
};

/* Generate random phone number */
static void gen_phone(char *buf, int nation_key)
{
    snprintf(buf, 16, "%02d-%03d-%03d-%04d",
             10 + nation_key,
             rand() % 1000,
             rand() % 1000,
             rand() % 10000);
}

/* Generate random text */
static void gen_text(char *buf, int min_len, int max_len)
{
    static const char *words[] = {
        "the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog",
        "special", "requests", "about", "pending", "packages", "detect",
        "slyly", "final", "accounts", "wake", "along", "bold", "deposits",
        "furiously", "ironic", "ideas", "cajole", "blithely", "according",
        "express", "unusual", "theodolites", "haggle", "carefully"
    };
    int num_words = sizeof(words) / sizeof(words[0]);
    int len = min_len + rand() % (max_len - min_len + 1);
    int pos = 0;

    while (pos < len - 1) {
        const char *word = words[rand() % num_words];
        int wlen = strlen(word);
        if (pos + wlen + 1 >= len) break;

        if (pos > 0) {
            buf[pos++] = ' ';
        }
        memcpy(buf + pos, word, wlen);
        pos += wlen;
    }
    buf[pos] = '\0';
}

/* Generate region.tbl */
static int gen_region(const char *output_dir)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/region.tbl", output_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    for (int i = 0; i < SF_1GB_REGION; i++) {
        fprintf(fp, "%d|%s|comment for region %d|\n",
                i, regions[i], i);
    }

    fclose(fp);
    LOG_INFO("Generated region.tbl (%d rows)", SF_1GB_REGION);
    return 0;
}

/* Generate nation.tbl */
static int gen_nation(const char *output_dir)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/nation.tbl", output_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    for (int i = 0; i < SF_1GB_NATION; i++) {
        fprintf(fp, "%d|%s|%d|comment for nation %d|\n",
                i, nations[i], nation_regions[i], i);
    }

    fclose(fp);
    LOG_INFO("Generated nation.tbl (%d rows)", SF_1GB_NATION);
    return 0;
}

/* Generate supplier.tbl */
static int gen_supplier(const char *output_dir, int scale_factor)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/supplier.tbl", output_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    int64_t num_rows = SF_1GB_SUPPLIER * scale_factor;
    char phone[16];
    char comment[128];

    ProgressBar *pb = progress_bar_create(num_rows, 50);

    for (int64_t i = 1; i <= num_rows; i++) {
        int nation = rand() % SF_1GB_NATION;
        gen_phone(phone, nation);
        gen_text(comment, 25, 100);

        fprintf(fp, "%ld|Supplier#%09ld|%s|%d|%s|%.2f|%s|\n",
                i, i,
                "123 Main Street",
                nation,
                phone,
                (rand() % 1000000) / 100.0 - 5000.0,
                comment);

        if (i % 1000 == 0) progress_bar_update(pb, i);
    }

    progress_bar_finish(pb);
    progress_bar_free(pb);
    fclose(fp);
    LOG_INFO("Generated supplier.tbl (%ld rows)", num_rows);
    return 0;
}

/* Generate part.tbl */
static int gen_part(const char *output_dir, int scale_factor)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/part.tbl", output_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    int64_t num_rows = SF_1GB_PART * scale_factor;
    char type[64];
    char name[64];

    ProgressBar *pb = progress_bar_create(num_rows, 50);

    for (int64_t i = 1; i <= num_rows; i++) {
        snprintf(type, sizeof(type), "%s %s %s",
                 types_syllable1[rand() % 6],
                 types_syllable2[rand() % 5],
                 types_syllable3[rand() % 5]);

        snprintf(name, sizeof(name), "Part #%ld %s", i,
                 types_syllable3[rand() % 5]);

        fprintf(fp, "%ld|%s|Manufacturer#%d|%s|%s|%d|%s|%.2f|part comment|\n",
                i,
                name,
                1 + rand() % 5,
                brands[rand() % 25],
                type,
                1 + rand() % 50,
                containers[rand() % 40],
                (900.0 + (i / 10.0)) * (1.0 + (rand() % 100) / 1000.0));

        if (i % 10000 == 0) progress_bar_update(pb, i);
    }

    progress_bar_finish(pb);
    progress_bar_free(pb);
    fclose(fp);
    LOG_INFO("Generated part.tbl (%ld rows)", num_rows);
    return 0;
}

/* Generate partsupp.tbl */
static int gen_partsupp(const char *output_dir, int scale_factor)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/partsupp.tbl", output_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    int64_t num_parts = SF_1GB_PART * scale_factor;
    int64_t num_suppliers = SF_1GB_SUPPLIER * scale_factor;
    int64_t num_rows = num_parts * 4;  /* 4 suppliers per part */
    int64_t row = 0;
    char comment[256];

    ProgressBar *pb = progress_bar_create(num_rows, 50);

    for (int64_t p = 1; p <= num_parts; p++) {
        for (int s = 0; s < 4; s++) {
            int64_t supp = ((p + s * (num_suppliers / 4)) % num_suppliers) + 1;
            gen_text(comment, 49, 198);

            fprintf(fp, "%ld|%ld|%d|%.2f|%s|\n",
                    p,
                    supp,
                    rand() % 10000,
                    (rand() % 100000) / 100.0,
                    comment);

            row++;
            if (row % 50000 == 0) progress_bar_update(pb, row);
        }
    }

    progress_bar_finish(pb);
    progress_bar_free(pb);
    fclose(fp);
    LOG_INFO("Generated partsupp.tbl (%ld rows)", num_rows);
    return 0;
}

/* Generate customer.tbl */
static int gen_customer(const char *output_dir, int scale_factor)
{
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/customer.tbl", output_dir);

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;

    int64_t num_rows = SF_1GB_CUSTOMER * scale_factor;
    char phone[16];
    char comment[128];

    ProgressBar *pb = progress_bar_create(num_rows, 50);

    for (int64_t i = 1; i <= num_rows; i++) {
        int nation = rand() % SF_1GB_NATION;
        gen_phone(phone, nation);
        gen_text(comment, 29, 116);

        fprintf(fp, "%ld|Customer#%09ld|%s|%d|%s|%.2f|%s|%s|\n",
                i, i,
                "456 Oak Avenue",
                nation,
                phone,
                (rand() % 1000000) / 100.0 - 5000.0,
                segments[rand() % 5],
                comment);

        if (i % 10000 == 0) progress_bar_update(pb, i);
    }

    progress_bar_finish(pb);
    progress_bar_free(pb);
    fclose(fp);
    LOG_INFO("Generated customer.tbl (%ld rows)", num_rows);
    return 0;
}

/* Generate orders.tbl and lineitem.tbl together */
static int gen_orders_lineitem(const char *output_dir, int scale_factor)
{
    char orders_path[MAX_PATH_LEN];
    char lineitem_path[MAX_PATH_LEN];
    snprintf(orders_path, sizeof(orders_path), "%s/orders.tbl", output_dir);
    snprintf(lineitem_path, sizeof(lineitem_path), "%s/lineitem.tbl", output_dir);

    FILE *orders_fp = fopen(orders_path, "w");
    FILE *lineitem_fp = fopen(lineitem_path, "w");
    if (!orders_fp || !lineitem_fp) {
        if (orders_fp) fclose(orders_fp);
        if (lineitem_fp) fclose(lineitem_fp);
        return -1;
    }

    int64_t num_orders = SF_1GB_ORDERS * scale_factor;
    int64_t num_customers = SF_1GB_CUSTOMER * scale_factor;
    int64_t num_parts = SF_1GB_PART * scale_factor;
    int64_t num_suppliers = SF_1GB_SUPPLIER * scale_factor;
    int64_t lineitem_count = 0;

    /* Date range: 1992-01-01 to 1998-12-31 (7 years) */
    time_t start_date = 694224000;  /* 1992-01-01 */
    int date_range = 2557;  /* days in 7 years */

    char order_comment[128];
    char lineitem_comment[64];

    ProgressBar *pb = progress_bar_create(num_orders, 50);

    for (int64_t o = 1; o <= num_orders; o++) {
        int64_t custkey = (rand() % num_customers) + 1;
        time_t order_date = start_date + (rand() % date_range) * 86400;
        struct tm *tm = gmtime(&order_date);
        char date_str[16];
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", tm);

        int num_lineitems = 1 + rand() % 7;  /* 1-7 line items per order */
        double total_price = 0;

        gen_text(order_comment, 19, 78);

        /* Generate line items first to calculate total */
        char lineitems[7][512];

        for (int l = 1; l <= num_lineitems; l++) {
            int64_t partkey = (rand() % num_parts) + 1;
            int64_t suppkey = (rand() % num_suppliers) + 1;
            int quantity = 1 + rand() % 50;
            double price = 900.0 + (partkey % 200001) / 1000.0;
            double discount = (rand() % 11) / 100.0;
            double tax = (rand() % 9) / 100.0;
            double extended = price * quantity;
            total_price += extended * (1 - discount) * (1 + tax);

            /* Ship date is 1-121 days after order date */
            time_t ship_date = order_date + (1 + rand() % 121) * 86400;
            time_t commit_date = order_date + (30 + rand() % 60) * 86400;
            time_t receipt_date = ship_date + (1 + rand() % 30) * 86400;

            struct tm *tm_ship = gmtime(&ship_date);
            struct tm *tm_commit = gmtime(&commit_date);
            struct tm *tm_receipt = gmtime(&receipt_date);

            char ship_str[16], commit_str[16], receipt_str[16];
            strftime(ship_str, sizeof(ship_str), "%Y-%m-%d", tm_ship);
            strftime(commit_str, sizeof(commit_str), "%Y-%m-%d", tm_commit);
            strftime(receipt_str, sizeof(receipt_str), "%Y-%m-%d", tm_receipt);

            char return_flag = ship_date > order_date + 93 * 86400 ? 'R' : (rand() % 2 ? 'A' : 'N');
            char line_status = ship_date < start_date + date_range * 86400 - 30 * 86400 ? 'F' : 'O';

            gen_text(lineitem_comment, 10, 43);

            snprintf(lineitems[l-1], sizeof(lineitems[0]),
                     "%ld|%ld|%ld|%d|%d|%.2f|%.2f|%.2f|%c|%c|%s|%s|%s|%s|%s|%s|\n",
                     o, partkey, suppkey, l,
                     quantity, extended, discount, tax,
                     return_flag, line_status,
                     ship_str, commit_str, receipt_str,
                     ship_instruct[rand() % 4],
                     ship_modes[rand() % 7],
                     lineitem_comment);
        }

        /* Write order */
        fprintf(orders_fp, "%ld|%ld|%c|%.2f|%s|%s|Clerk#%09ld|%d|%s|\n",
                o, custkey,
                rand() % 2 ? 'O' : (rand() % 2 ? 'F' : 'P'),
                total_price,
                date_str,
                priorities[rand() % 5],
                (int64_t)(1 + rand() % 1000),
                0,
                order_comment);

        /* Write line items */
        for (int l = 0; l < num_lineitems; l++) {
            fputs(lineitems[l], lineitem_fp);
            lineitem_count++;
        }

        if (o % 50000 == 0) progress_bar_update(pb, o);
    }

    progress_bar_finish(pb);
    progress_bar_free(pb);
    fclose(orders_fp);
    fclose(lineitem_fp);

    LOG_INFO("Generated orders.tbl (%ld rows)", num_orders);
    LOG_INFO("Generated lineitem.tbl (%ld rows)", lineitem_count);
    return 0;
}

/* Main data generation function */
int tpch_generate_data(int scale_factor, const char *output_dir)
{
    LOG_INFO("Generating TPC-H data (Scale Factor: %d)", scale_factor);
    LOG_INFO("Output directory: %s", output_dir);

    /* Create output directory */
    mkdir(output_dir, 0755);

    srand(time(NULL));

    /* Generate all tables */
    if (gen_region(output_dir) != 0) return -1;
    if (gen_nation(output_dir) != 0) return -1;
    if (gen_supplier(output_dir, scale_factor) != 0) return -1;
    if (gen_part(output_dir, scale_factor) != 0) return -1;
    if (gen_partsupp(output_dir, scale_factor) != 0) return -1;
    if (gen_customer(output_dir, scale_factor) != 0) return -1;
    if (gen_orders_lineitem(output_dir, scale_factor) != 0) return -1;

    LOG_INFO("Data generation complete");
    return 0;
}
